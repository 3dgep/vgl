// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// What the interactive samples share besides navigation: their places, the camera's field of
// view and the clear colour, and the checks that --check runs on what a backend draws. The
// checks know images, not graphics APIs: each sample reads its render target back into an image
// and hands it over.

#ifndef VGL_SAMPLES_SAMPLE_CHECK_HPP
#define VGL_SAMPLES_SAMPLE_CHECK_HPP

#include "sample_camera.hpp"

#include <vgl/vgl.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sample
{

constexpr double field_of_view = 60.0; // Degrees, vertical.
constexpr std::array<float, 3> clear_color{0.02f, 0.02f, 0.05f};

// A place to jump to (keys 1 to 7) and to check, with the share of a 16:9 view the globe covers
// there: from orbit, the globe is a disc in the middle; closer, the terrain fills the view below
// the horizon.
struct place
{
    const char* name;
    sample::camera camera;
    double least_coverage;
    double most_coverage;
};

inline constexpr std::array<place, 7> places{{
    {"orbit, Atlantic", {30.0, -30.0, 2.0e7, 0.0, 90.0}, 0.06, 0.12},
    {"Europe", {48.0, 10.0, 5.0e6, 0.0, 90.0}, 0.45, 0.70},
    {"Alps", {44.5, 9.0, 3.0e5, 0.0, 35.0}, 0.65, 0.82},
    {"Himalaya", {26.0, 86.9, 6.0e4, 0.0, 20.0}, 0.58, 0.77},
    {"Mid-Atlantic Ridge", {20.0, -50.0, 1.5e6, 90.0, 40.0}, 0.38, 0.55},
    {"Sahara", {23.0, 12.0, 1.0e6, 0.0, 90.0}, 0.99, 1.0},
    {"Sognefjord", {61.1, 6.2, 1.5e4, 60.0, 25.0}, 0.75, 0.9},
}};

// A contour interval for an altitude: about a 250th of it, rounded down to 1, 2 or 5 times a
// power of ten, from 1 m to 2,000 m. From 300 km that is 1,000 m, from 60 km 200 m.
float contour_interval_for(double altitude);

const char* depth_name(vgl_depth_convention depth);

// True when nothing loads, builds or uploads, and every selected tile is drawn.
bool idle(const vgl_context_statistics& s);

// RGBA, rows from the top.
struct image
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

// Writes an image as a PPM file.
void save_image(const image& i, const std::string& path);

// The path of a saved image: the directory, the depth convention and a name, with spaces and
// commas replaced.
std::string image_path(const std::string& directory, vgl_depth_convention depth, const char* name);

// Whether a pixel shows the globe rather than the clear colour.
bool covered_pixel(const image& i, int x, int y);

// The share of pixels that show the globe.
double coverage(const image& i);

// The share of pixels whose colour differs by more than a little.
double difference(const image& a, const image& b);

// Records a failed check, with what failed; failures() counts them.
void expect(bool condition, const std::string& what);
int failures();

// The checks of one place's image once loading has settled: it settled, every selected tile is
// drawn, from orbit with coarse meshes, the globe covers its share of the view, below and with
// the sky above, and the Sahara is sand-coloured. It prints a line with what was drawn.
void check_place(vgl_depth_convention depth,
                 const place& p,
                 int frames,
                 bool settled,
                 const vgl_context_statistics& s,
                 const image& pixels);

// Where the conventions resolve depth well, they draw the same image: images[c][i] is the
// image of conventions[c] at places[i], and conventions[0] is the reference. A conventional
// depth buffer loses distant detail, so it is held to a looser bound.
void compare_conventions(const std::vector<vgl_depth_convention>& conventions,
                         const std::vector<std::vector<image>>& images);

} // namespace sample

#endif // VGL_SAMPLES_SAMPLE_CHECK_HPP
