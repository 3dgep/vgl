// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "image_codec.hpp"

#include <gdal.h>

#include <array>
#include <atomic>
#include <cpl_conv.h>
#include <cpl_error.h>
#include <cpl_string.h>
#include <cpl_vsi.h>
#include <cstring>
#include <stdexcept>
#include <string>

namespace vgl::bake
{

namespace
{

// A file name in GDAL's in-memory file system, unique across threads.
std::string memory_path(const char* extension)
{
    static std::atomic<std::uint64_t> counter{0};
    return "/vsimem/vgl-bake-" + std::to_string(counter++) + extension;
}

std::string gdal_error()
{
    const char* message = CPLGetLastErrorMsg();
    return message != nullptr && *message != '\0' ? message : "unknown GDAL error";
}

// Interleaved pixels to and from the bands of a dataset.
CPLErr transfer(GDALDatasetH dataset, GDALRWFlag direction, image& img)
{
    const int channels = static_cast<int>(img.channels);
    const int width = static_cast<int>(img.width);
    return GDALDatasetRasterIO(dataset,
                               direction,
                               0,
                               0,
                               width,
                               static_cast<int>(img.height),
                               img.pixels.data(),
                               width,
                               static_cast<int>(img.height),
                               GDT_Byte,
                               channels,
                               nullptr,
                               channels,
                               width * channels,
                               1);
}

std::vector<std::byte> encode(const image& img, const char* driver_name, CSLConstList options)
{
    if (img.channels < 1 || img.channels > 4 ||
        img.pixels.size() != static_cast<std::size_t>(img.width) * img.height * img.channels)
    {
        throw std::logic_error("encode: the image's size and pixels disagree");
    }
    GDALDatasetH source = GDALCreate(GDALGetDriverByName("MEM"),
                                     "",
                                     static_cast<int>(img.width),
                                     static_cast<int>(img.height),
                                     static_cast<int>(img.channels),
                                     GDT_Byte,
                                     nullptr);
    if (source == nullptr)
    {
        throw std::runtime_error("encode: cannot create an image in memory: " + gdal_error());
    }
    image copy = img; // GDALDatasetRasterIO takes a non-const buffer, even for writing.
    if (transfer(source, GF_Write, copy) != CE_None)
    {
        GDALClose(source);
        throw std::runtime_error("encode: cannot fill an image in memory: " + gdal_error());
    }
    if (img.channels == 4)
    {
        GDALSetRasterColorInterpretation(GDALGetRasterBand(source, 4), GCI_AlphaBand);
    }
    const std::string path = memory_path(driver_name);
    GDALDatasetH file = GDALCreateCopy(
        GDALGetDriverByName(driver_name), path.c_str(), source, FALSE, options, nullptr, nullptr);
    GDALClose(source);
    if (file == nullptr)
    {
        throw std::runtime_error(std::string("encode: GDAL's ") + driver_name +
                                 " driver failed: " + gdal_error());
    }
    GDALClose(file);
    vsi_l_offset size = 0;
    GByte* data = VSIGetMemFileBuffer(path.c_str(), &size, TRUE);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size != 0)
    {
        std::memcpy(bytes.data(), data, bytes.size());
    }
    CPLFree(data);
    // Side-car files, should a driver write one.
    VSIUnlink((path + ".aux.xml").c_str());
    return bytes;
}

} // namespace

std::vector<std::byte> encode_jpeg(const image& img, int quality)
{
    if (img.channels != 1 && img.channels != 3)
    {
        throw std::logic_error("encode_jpeg: JPEG holds grey or RGB images");
    }
    const std::string q = "QUALITY=" + std::to_string(quality);
    const std::array<const char*, 2> options = {q.c_str(), nullptr};
    return encode(img, "JPEG", options.data());
}

std::vector<std::byte> encode_png(const image& img)
{
    const std::array<const char*, 2> options = {"ZLEVEL=9", nullptr};
    return encode(img, "PNG", options.data());
}

image decode_image(std::span<const std::byte> file)
{
    const std::string path = memory_path(".img");
    // GDAL reads the buffer in place; it does not modify it.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto* data = reinterpret_cast<GByte*>(const_cast<std::byte*>(file.data()));
    VSILFILE* handle = VSIFileFromMemBuffer(path.c_str(), data, file.size(), FALSE);
    if (handle == nullptr)
    {
        throw std::runtime_error("decode_image: cannot map the file in memory");
    }
    VSIFCloseL(handle);
    GDALDatasetH dataset =
        GDALOpenEx(path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (dataset == nullptr)
    {
        VSIUnlink(path.c_str());
        throw std::runtime_error("decode_image: not a readable image: " + gdal_error());
    }
    image img;
    img.width = static_cast<std::uint32_t>(GDALGetRasterXSize(dataset));
    img.height = static_cast<std::uint32_t>(GDALGetRasterYSize(dataset));
    img.channels = static_cast<std::uint32_t>(GDALGetRasterCount(dataset));
    img.pixels.resize(static_cast<std::size_t>(img.width) * img.height * img.channels);
    const CPLErr err =
        img.channels >= 1 && img.channels <= 4 ? transfer(dataset, GF_Read, img) : CE_Failure;
    GDALClose(dataset);
    VSIUnlink(path.c_str());
    if (err != CE_None)
    {
        throw std::runtime_error("decode_image: reading the pixels failed: " + gdal_error());
    }
    return img;
}

} // namespace vgl::bake
