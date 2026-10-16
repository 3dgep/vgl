// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "source.hpp"

#include <gdal.h>
#include <gdal_utils.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cpl_conv.h>
#include <cpl_error.h>
#include <limits>
#include <map>
#include <ogr_srs_api.h>
#include <stdexcept>
#include <utility>

namespace vgl::bake
{

namespace
{

constexpr double nan = std::numeric_limits<double>::quiet_NaN();

std::string last_gdal_error()
{
    const char* message = CPLGetLastErrorMsg();
    return message != nullptr && *message != '\0' ? message : "unknown GDAL error";
}

bool intersects(const degree_region& a, const degree_region& b) noexcept
{
    return a.west < b.east && b.west < a.east && a.south < b.north && b.south < a.north;
}

// Checks that a dataset uses geographic coordinates on the WGS84 ellipsoid.
bool is_geographic_wgs84(GDALDatasetH dataset)
{
    OGRSpatialReferenceH srs = GDALGetSpatialRef(dataset);
    if (srs == nullptr || OSRIsGeographic(srs) == 0)
    {
        return false;
    }
    OGRErr err = OGRERR_NONE;
    const double a = OSRGetSemiMajor(srs, &err);
    const double inv_f = OSRGetInvFlattening(srs, &err);
    return err == OGRERR_NONE && std::fabs(a - 6378137.0) < 1e-3 &&
           std::fabs(inv_f - 298.257223563) < 1e-6;
}

} // namespace

void initialize_gdal(std::size_t cache_megabytes)
{
    GDALAllRegister();
    CPLSetConfigOption("GDAL_CACHEMAX", std::to_string(cache_megabytes).c_str());
    // No side-car files next to sources or encoded tiles.
    CPLSetConfigOption("GDAL_PAM_ENABLED", "NO");
    // Errors are reported through return values and CPLGetLastErrorMsg.
    CPLSetErrorHandler(CPLQuietErrorHandler);
}

source::source(source_config config) : config_(std::move(config))
{
    for (const std::filesystem::path& path : config_.files)
    {
        GDALDatasetH dataset = GDALOpenEx(
            path.string().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (dataset == nullptr)
        {
            throw std::runtime_error("source \"" + config_.name + "\": cannot open " +
                                     path.string() + ": " + last_gdal_error());
        }
        std::array<double, 6> gt = {};
        const bool georeferenced = GDALGetGeoTransform(dataset, gt.data()) == CE_None;
        const bool geographic = is_geographic_wgs84(dataset);
        const int bands = GDALGetRasterCount(dataset);
        for (int b = 1; b <= bands; ++b)
        {
            byte_data_ =
                byte_data_ && GDALGetRasterDataType(GDALGetRasterBand(dataset, b)) == GDT_Byte;
        }
        const int width = GDALGetRasterXSize(dataset);
        const int height = GDALGetRasterYSize(dataset);
        // Overview sizes, so that the overview for a level can be chosen explicitly.
        std::vector<std::pair<int, int>> overviews;
        if (bands >= 1)
        {
            GDALRasterBandH band = GDALGetRasterBand(dataset, 1);
            for (int i = 0; i < GDALGetOverviewCount(band); ++i)
            {
                GDALRasterBandH o = GDALGetOverview(band, i);
                overviews.emplace_back(GDALGetRasterBandXSize(o), GDALGetRasterBandYSize(o));
            }
        }
        GDALClose(dataset);
        if (!georeferenced || gt[2] != 0.0 || gt[4] != 0.0 || gt[1] <= 0.0 || gt[5] >= 0.0)
        {
            throw std::runtime_error("source \"" + config_.name + "\": " + path.string() +
                                     " must be a north-up raster with a geotransform");
        }
        if (!geographic)
        {
            throw std::runtime_error(
                "source \"" + config_.name + "\": " + path.string() +
                " must use geographic coordinates on WGS84 (such as EPSG:4326); "
                "reproject it with gdalwarp first");
        }
        if (bands < 1)
        {
            throw std::runtime_error("source \"" + config_.name + "\": " + path.string() +
                                     " has no raster band");
        }
        if (bands_ != 0 && bands != bands_)
        {
            throw std::runtime_error("source \"" + config_.name + "\": " + path.string() + " has " +
                                     std::to_string(bands) +
                                     " bands, unlike the source's other files");
        }
        bands_ = bands;
        source_file f;
        f.path = path;
        f.bounds = {gt[0], gt[3] + gt[5] * height, gt[0] + gt[1] * width, gt[3]};
        f.pixel_width = gt[1];
        f.pixel_height = -gt[5];
        f.pixel_size = std::min(f.pixel_width, f.pixel_height);
        for (const auto& [ow, oh] : overviews)
        {
            f.overview_pixel_width.push_back(f.pixel_width * width / ow);
            f.overview_pixel_height.push_back(f.pixel_height * height / oh);
        }
        finest_pixel_size_ = std::min(finest_pixel_size_, f.pixel_size);
        coarsest_pixel_size_ = std::max({coarsest_pixel_size_, f.pixel_width, f.pixel_height});
        files_.push_back(f);
    }
}

bool source::covers(const degree_region& r) const noexcept
{
    return std::any_of(files_.begin(), files_.end(), [&](const source_file& f) {
        return intersects(f.bounds, r);
    });
}

struct sampler::impl
{
    std::map<std::filesystem::path, GDALDatasetH> datasets;
    GDALDriverH memory_driver = GDALGetDriverByName("MEM");
    std::string wgs84_wkt;

    impl()
    {
        OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(srs, 4326);
        OSRSetAxisMappingStrategy(srs, OAMS_TRADITIONAL_GIS_ORDER);
        char* wkt = nullptr;
        OSRExportToWkt(srs, &wkt);
        wgs84_wkt = wkt;
        CPLFree(wkt);
        OSRDestroySpatialReference(srs);
    }

    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;
    impl(impl&&) = delete;
    impl& operator=(impl&&) = delete;

    ~impl()
    {
        for (auto& [path, dataset] : datasets)
        {
            GDALClose(dataset);
        }
    }

    GDALDatasetH open(const std::filesystem::path& path)
    {
        auto it = datasets.find(path);
        if (it != datasets.end())
        {
            return it->second;
        }
        GDALDatasetH dataset = GDALOpenEx(
            path.string().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (dataset == nullptr)
        {
            throw std::runtime_error("cannot open " + path.string() + ": " + last_gdal_error());
        }
        datasets.emplace(path, dataset);
        return dataset;
    }

    // Area filter: each sample is the average (or maximum) of the source pixels whose centres
    // lie in its footprint, the box of one spacing around it, half-open towards the east and
    // the south. That is a function of the sample's position only, and pixels of adjacent
    // files add up as if they were one raster. GDAL's warper is not used here: its averages
    // depend on more than the position, which gave different values for shared borders.
    void filter_grid(const std::vector<const source_file*>& inputs,
                     double first_lon,
                     double first_lat,
                     double step,
                     int nx,
                     int ny,
                     bool maximum,
                     int band_number,
                     std::vector<double>& values)
    {
        const std::size_t count = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
        std::vector<double> sum(count, 0.0);
        std::vector<std::uint32_t> pixels(count, 0);
        std::vector<double> row;
        for (const source_file* f : inputs)
        {
            // Averages read the coarsest overview with at least two pixels across a footprint,
            // so that every footprint holds pixel centres, whatever the half-open box does at
            // its edges. The maximum reads the full resolution.
            GDALRasterBandH band = GDALGetRasterBand(open(f->path), band_number);
            int has_nodata = 0;
            const double nodata = GDALGetRasterNoDataValue(band, &has_nodata);
            double pw = f->pixel_width;
            double ph = f->pixel_height;
            if (!maximum)
            {
                for (std::size_t o = 0; o < f->overview_pixel_width.size(); ++o)
                {
                    if (2.0 * std::max(f->overview_pixel_width[o], f->overview_pixel_height[o]) <=
                        step)
                    {
                        band = GDALGetOverview(GDALGetRasterBand(open(f->path), band_number),
                                               static_cast<int>(o));
                        pw = f->overview_pixel_width[o];
                        ph = f->overview_pixel_height[o];
                    }
                }
            }
            const int width = GDALGetRasterBandXSize(band);
            const int height = GDALGetRasterBandYSize(band);
            const double ox = f->bounds.west;
            const double oy = f->bounds.north;

            // Pixel c has its centre at ox + (c + 0.5) pw; it belongs to the footprint
            // [lon - step / 2, lon + step / 2) if c lies in [first, last).
            const auto first_column = [&](double lon) {
                return static_cast<int>(std::ceil((lon - step / 2.0 - ox) / pw - 0.5));
            };
            const auto first_row = [&](double lat) {
                return static_cast<int>(std::ceil((oy - (lat + step / 2.0)) / ph - 0.5));
            };
            const int c_begin = std::clamp(first_column(first_lon), 0, width);
            const int c_end = std::clamp(first_column(first_lon + nx * step), 0, width);
            if (c_begin >= c_end)
            {
                continue;
            }
            for (int j = 0; j < ny; ++j)
            {
                const double lat = first_lat - j * step;
                const int r_begin = std::clamp(first_row(lat), 0, height);
                const int r_end = std::clamp(first_row(lat - step), 0, height);
                if (r_begin >= r_end)
                {
                    continue;
                }
                const int w = c_end - c_begin;
                row.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(r_end - r_begin));
                if (GDALRasterIO(band,
                                 GF_Read,
                                 c_begin,
                                 r_begin,
                                 w,
                                 r_end - r_begin,
                                 row.data(),
                                 w,
                                 r_end - r_begin,
                                 GDT_Float64,
                                 0,
                                 0) != CE_None)
                {
                    throw std::runtime_error("reading " + f->path.string() +
                                             " failed: " + last_gdal_error());
                }
                for (int i = 0; i < nx; ++i)
                {
                    const double lon = first_lon + i * step;
                    const int a = std::clamp(first_column(lon), c_begin, c_end);
                    const int b = std::clamp(first_column(lon + step), c_begin, c_end);
                    const std::size_t k =
                        static_cast<std::size_t>(j) * static_cast<std::size_t>(nx) +
                        static_cast<std::size_t>(i);
                    for (int r = 0; r < r_end - r_begin; ++r)
                    {
                        for (int c = a; c < b; ++c)
                        {
                            const double v =
                                row[static_cast<std::size_t>(r) * static_cast<std::size_t>(w) +
                                    static_cast<std::size_t>(c - c_begin)];
                            if ((has_nodata != 0 && v == nodata) || std::isnan(v))
                            {
                                continue;
                            }
                            if (pixels[k] == 0)
                            {
                                sum[k] = v;
                            }
                            else
                            {
                                sum[k] = maximum ? std::max(sum[k], v) : sum[k] + v;
                            }
                            ++pixels[k];
                        }
                    }
                }
            }
        }
        for (std::size_t k = 0; k < count; ++k)
        {
            if (pixels[k] != 0)
            {
                values[k] = maximum ? sum[k] : sum[k] / pixels[k];
            }
        }
    }

    // Samples a grid of nx x ny points, with the first at (first_lon, first_lat) and spacing
    // step, into values (row-major, NaN where there is no data).
    void sample_grid(const source& s,
                     double first_lon,
                     double first_lat,
                     double step,
                     int nx,
                     int ny,
                     const std::string& resampling,
                     std::vector<double>& values,
                     int band_number)
    {
        values.assign(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), nan);

        // Files whose pixels can influence the grid: its extent plus a margin for the
        // resampling kernel and the footprint of a sample.
        const double margin = 4.0 * std::max(step, s.coarsest_pixel_size());
        const degree_region extent{first_lon - margin,
                                   first_lat - (ny - 1) * step - margin,
                                   first_lon + (nx - 1) * step + margin,
                                   first_lat + margin};
        std::vector<const source_file*> inputs;
        for (const source_file& f : s.files())
        {
            if (intersects(f.bounds, extent))
            {
                inputs.push_back(&f);
            }
        }
        if (inputs.empty())
        {
            return;
        }
        if (resampling == "average" || resampling == "max")
        {
            filter_grid(inputs,
                        first_lon,
                        first_lat,
                        step,
                        nx,
                        ny,
                        resampling == "max",
                        band_number,
                        values);
            return;
        }

        // Pixel centres on the sample positions.
        GDALDatasetH target = GDALCreate(memory_driver, "", nx, ny, 1, GDT_Float64, nullptr);
        std::array<double, 6> gt = {
            first_lon - step / 2.0, step, 0.0, first_lat + step / 2.0, 0.0, -step};
        GDALSetGeoTransform(target, gt.data());
        GDALSetProjection(target, wgs84_wkt.c_str());
        GDALRasterBandH band = GDALGetRasterBand(target, 1);
        GDALSetRasterNoDataValue(band, nan);
        GDALFillRaster(band, nan, 0.0);

        // Values must depend only on positions:
        // - An exact transform (-et 0).
        // - Kernel resampling (bilinear, cubic) at full resolution, with the kernel scale fixed
        //   by XSCALE and YSCALE. GDAL otherwise derives the scale from the shape of each
        //   processing chunk, which differs near the edges of a source and changes the values.
        //   Each file is warped on its own, in the same order for every tile, so each gets the
        //   scale of its own resolution; later files fill in where earlier ones had no data.
        // Area filters do not come here (see filter_grid).
        for (std::size_t i = 0; i < inputs.size(); ++i)
        {
            const source_file& f = *inputs[i];
            const std::string overview = "NONE";
            const std::string source_band = std::to_string(band_number);
            const std::string xscale = "XSCALE=" + std::to_string(f.pixel_width / step);
            const std::string yscale = "YSCALE=" + std::to_string(f.pixel_height / step);
            std::vector<const char*> arguments = {
                "-r",       resampling.c_str(),  "-et",        "0",
                "-s_srs",   "EPSG:4326",         "-t_srs",     "EPSG:4326",
                "-ovr",     overview.c_str(),    "-dstnodata", "nan",
                "-wo",      xscale.c_str(),      "-wo",        yscale.c_str(),
                "-srcband", source_band.c_str(), "-dstband",   "1",
                "-q"};
            if (i == 0)
            {
                arguments.insert(arguments.end(), {"-wo", "INIT_DEST=NO_DATA"});
            }
            arguments.push_back(nullptr);
            // GDAL takes the arguments as char**, but does not modify them.
            // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast)
            GDALWarpAppOptions* options =
                GDALWarpAppOptionsNew(const_cast<char**>(arguments.data()), nullptr);
            // NOLINTEND(cppcoreguidelines-pro-type-const-cast)
            GDALDatasetH input = open(f.path);
            int usage_error = 0;
            CPLErrorReset();
            GDALDatasetH result = GDALWarp(nullptr, target, 1, &input, options, &usage_error);
            GDALWarpAppOptionsFree(options);
            if (result == nullptr)
            {
                const std::string message = last_gdal_error();
                GDALClose(target);
                throw std::runtime_error("source \"" + s.config().name + "\": resampling " +
                                         f.path.string() + " failed: " + message);
            }
        }
        const CPLErr err =
            GDALRasterIO(band, GF_Read, 0, 0, nx, ny, values.data(), nx, ny, GDT_Float64, 0, 0);
        GDALClose(target);
        if (err != CE_None)
        {
            throw std::runtime_error("source \"" + s.config().name +
                                     "\": reading resampled values failed: " + last_gdal_error());
        }
    }
};

sampler::sampler() : impl_(std::make_unique<impl>()) {}

sampler::~sampler() = default;

void sampler::sample(const source& s,
                     const vgl::internal::tile_id& t,
                     std::uint32_t size,
                     const std::string& resampling,
                     std::vector<double>& values,
                     int band)
{
    const degree_region b = vgl::internal::tile_bounds(t);
    const double step = vgl::internal::tile_span(t.level) / (size - 1);
    impl_->sample_grid(s,
                       b.west,
                       b.north,
                       step,
                       static_cast<int>(size),
                       static_cast<int>(size),
                       resampling,
                       values,
                       band);
}

void sampler::sample_grid(const source& s,
                          double first_longitude,
                          double first_latitude,
                          double step,
                          std::uint32_t nx,
                          std::uint32_t ny,
                          const std::string& resampling,
                          std::vector<double>& values,
                          int band)
{
    impl_->sample_grid(s,
                       first_longitude,
                       first_latitude,
                       step,
                       static_cast<int>(nx),
                       static_cast<int>(ny),
                       resampling,
                       values,
                       band);
}

} // namespace vgl::bake
