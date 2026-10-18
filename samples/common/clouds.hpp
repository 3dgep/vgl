// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The samples' clouds: NASA's Blue Marble cloud composite (samples/data/clouds.jpg), a grey
// image of the whole globe in latitude and longitude, read with stb_image and given to the
// backends, which draw it as a shell above the ground.

#ifndef VGL_SAMPLES_CLOUDS_HPP
#define VGL_SAMPLES_CLOUDS_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace sample
{

struct cloud_image
{
    std::vector<std::uint8_t> coverage; // One byte per pixel, rows from the north.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Reads the clouds of the sample set in a directory (its clouds.jpg) for sky_clouds; false, with
// a message, if the file cannot be read.
bool clouds_load(const std::string& directory, std::string& error);

// The clouds clouds_load read, or none, with width 0.
const cloud_image& sky_clouds();

} // namespace sample

#endif // VGL_SAMPLES_CLOUDS_HPP
