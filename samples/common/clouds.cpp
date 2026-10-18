// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "clouds.hpp"

// A private copy of stb_image's JPEG decoder: static, so that it cannot clash with the one
// that a static vgl holds.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#if defined(__GNUC__) || defined(__clang__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_image.h>
#if defined(__GNUC__) || defined(__clang__)
#    pragma GCC diagnostic pop
#endif

#include <fstream>
#include <iterator>

namespace sample
{

namespace
{

cloud_image& loaded()
{
    static cloud_image image;
    return image;
}

} // namespace

bool clouds_load(const std::string& directory, std::string& error)
{
    const std::string path = directory + "/clouds.jpg";
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "cannot open " + path;
        return false;
    }
    const std::vector<char> file{std::istreambuf_iterator<char>(in),
                                 std::istreambuf_iterator<char>()};
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(file.data()),
                                            static_cast<int>(file.size()),
                                            &width,
                                            &height,
                                            &channels,
                                            1);
    if (pixels == nullptr)
    {
        error = path + " is not a JPEG image";
        return false;
    }
    cloud_image& image = loaded();
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.coverage.assign(pixels, pixels + static_cast<std::size_t>(width) * height);
    stbi_image_free(pixels);
    return true;
}

const cloud_image& sky_clouds()
{
    return loaded();
}

} // namespace sample
