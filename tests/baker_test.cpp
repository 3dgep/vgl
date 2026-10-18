// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Baker tests on synthetic, analytically defined surfaces (design document, section 12.2):
// heights, seams, datum conversion, determinism and resuming.

#include "bake_config.hpp"
#include "baker.hpp"
#include "decoders/elevation_codec.hpp"
#include "image_codec.hpp"
#include "inspect.hpp"
#include "pack_file.hpp"
#include "source.hpp"
#include "vgl_test.hpp"

#include <gdal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <ogr_srs_api.h>
#include <random>
#include <sstream>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using namespace vgl::bake;

constexpr double geoid_height = 12.5;

// The synthetic terrain, in metres above the geoid, at a position in degrees.
double surface(double latitude, double longitude)
{
    return 500.0 + 300.0 * std::sin(3.0 * longitude) * std::cos(2.0 * latitude) + 20.0 * longitude;
}

// Writes a north-up GeoTIFF in EPSG:4326 with pixels of pw x ph degrees, whose pixel centres
// sample value(latitude, longitude).
template <typename Value>
void write_raster_cells(const fs::path& path,
                        double west,
                        double north,
                        double pw,
                        double ph,
                        int width,
                        int height,
                        Value value)
{
    GDALDriverH driver = GDALGetDriverByName("GTiff");
    GDALDatasetH ds =
        GDALCreate(driver, path.string().c_str(), width, height, 1, GDT_Float32, nullptr);
    double gt[6] = {west, pw, 0.0, north, 0.0, -ph};
    GDALSetGeoTransform(ds, gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    GDALSetSpatialRef(ds, srs);
    OSRDestroySpatialReference(srs);
    std::vector<float> row(static_cast<std::size_t>(width));
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    for (int j = 0; j < height; ++j)
    {
        for (int i = 0; i < width; ++i)
        {
            row[static_cast<std::size_t>(i)] =
                static_cast<float>(value(north - (j + 0.5) * ph, west + (i + 0.5) * pw));
        }
        if (GDALRasterIO(band, GF_Write, 0, j, width, 1, row.data(), width, 1, GDT_Float32, 0, 0) !=
            CE_None)
        {
            throw std::runtime_error("cannot write " + path.string());
        }
    }
    GDALClose(ds);
}

// A raster with square pixels.
template <typename Value>
void write_raster(const fs::path& path,
                  double west,
                  double north,
                  double pixel,
                  int width,
                  int height,
                  Value value)
{
    write_raster_cells(path, west, north, pixel, pixel, width, height, value);
}

void write_text(const fs::path& path, const std::string& text)
{
    std::ofstream out(path);
    out << text;
}

// A scratch directory with a constant geoid pack and the synthetic terrain source.
struct fixture
{
    fs::path dir;

    fixture()
    {
        dir = fs::temp_directory_path() /
              ("vgl-baker-test-" + std::to_string(std::random_device{}()));
        fs::create_directories(dir);
        initialize_gdal(256);

        // A constant geoid, so that converted heights are easy to predict.
        write_raster(dir / "geoid.tif", -180.0, 90.0, 1.0, 360, 180, [](double, double) {
            return geoid_height;
        });
        write_text(dir / "geoid.json", R"({
            "output": "geoid", "layer": "geoid", "tile_size": 9, "min_level": 0, "max_level": 2,
            "shard_level": 3, "sources": [{"files": "geoid.tif", "datum": "ellipsoid"}]
        })");
        bake(load_bake_config(dir / "geoid.json"), {});

        // Terrain at 0.001 degree spacing over 10.4 to 11.1 E, 60.4 to 61.1 N.
        write_raster(dir / "terrain.tif", 10.4, 61.1, 0.001, 700, 700, surface);
    }

    ~fixture()
    {
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }

    // Bakes the terrain into output and returns the configuration.
    bake_config bake_terrain(const std::string& output, const bake_options& options = {}) const
    {
        write_text(dir / (output + ".json"), R"({
            "output": ")" + output + R"(", "tile_size": 17, "min_level": 8, "max_level": 13,
            "shard_level": 10, "region": [10.5, 60.5, 11.0, 61.0], "geoid": "geoid",
            "metadata": {"name": "Synthetic", "attribution": "Test data"},
            "sources": [{"files": "terrain.tif", "datum": "egm2008"}]
        })");
        bake_config c = load_bake_config(dir / (output + ".json"));
        bake(c, options);
        return c;
    }
};

std::vector<std::pair<fs::path, std::vector<std::byte>>> pack_files(const fs::path& pack)
{
    std::vector<std::pair<fs::path, std::vector<std::byte>>> files;
    for (const auto& entry : fs::recursive_directory_iterator(pack))
    {
        if (entry.is_regular_file() && entry.path().string().find(".bake") == std::string::npos)
        {
            files.emplace_back(fs::relative(entry.path(), pack), read_file(entry.path()));
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

TEST_CASE(baked_heights_match_the_surface_with_datum_conversion)
{
    const fixture f;
    f.bake_terrain("terrain");

    const pack_file p(f.dir / "terrain");
    CHECK_EQ(p.manifest().layer().max_level, static_cast<std::uint8_t>(13));
    CHECK(p.manifest().metadata_value("attribution") == "Test data");
    height_query q(p);

    // At level 13 (0.0014 degree spacing) the baker interpolates the source; heights are the
    // surface plus the geoid height, to within quantization (0.0625 m) and interpolation error.
    std::mt19937_64 rng(12);
    std::uniform_real_distribution<double> lat(60.52, 60.98);
    std::uniform_real_distribution<double> lon(10.52, 10.98);
    double worst = 0.0;
    for (int i = 0; i < 2000; ++i)
    {
        const double la = lat(rng);
        const double lo = lon(rng);
        const height_query::result r = q.height(la, lo);
        REQUIRE(r.level == 13);
        worst = std::max(worst, std::fabs(r.height - (surface(la, lo) + geoid_height)));
    }
    CHECK(worst < 0.1);
    std::printf("synthetic bake: largest height error %.4f m\n", worst);
}

TEST_CASE(baked_pack_passes_validation)
{
    const fixture f;
    f.bake_terrain("terrain");
    std::ostringstream report;
    const validation_report r = validate(f.dir / "terrain", report);
    CHECK(r.ok());
    CHECK(r.tiles > 500);
    if (!r.ok())
    {
        std::fputs(report.str().c_str(), stderr);
    }
}

TEST_CASE(bakes_are_deterministic_and_resumable)
{
    const fixture f;
    f.bake_terrain("first");
    bake_options parallel;
    parallel.jobs = 3;
    f.bake_terrain("second", parallel);
    CHECK(pack_files(f.dir / "first") == pack_files(f.dir / "second"));

    // Remove one shard's record and file, and the manifest, then resume.
    fs::path record;
    for (const auto& entry : fs::directory_iterator(f.dir / "second" / ".bake"))
    {
        if (entry.path().filename().string().rfind("shard-", 0) == 0)
        {
            record = entry.path();
            break;
        }
    }
    REQUIRE(!record.empty());
    fs::remove(record);
    fs::remove(f.dir / "second" / "pack.vglm");
    bake_options resume;
    resume.resume = true;
    bake(load_bake_config(f.dir / "second.json"), resume);
    CHECK(pack_files(f.dir / "first") == pack_files(f.dir / "second"));

    // Without --resume, an existing bake is refused.
    bool refused = false;
    try
    {
        bake(load_bake_config(f.dir / "second.json"), {});
    }
    catch (const std::runtime_error&)
    {
        refused = true;
    }
    CHECK(refused);
}

TEST_CASE(filtered_levels_are_seamless_across_files)
{
    // Two adjacent source files with different resolutions, like GLO-30's latitude bands, and
    // only filtered levels (spacings well above the source pixels).
    const fixture f;
    write_raster(f.dir / "west.tif", 10.4, 61.1, 0.001, 350, 700, surface);
    write_raster(f.dir / "east.tif", 10.75, 61.1, 0.002, 175, 350, surface);
    for (const char* filter : {"average", "maximum"})
    {
        const std::string name = std::string("split-") + filter;
        write_text(f.dir / (name + ".json"),
                   R"({
            "output": ")" +
                       name + R"(", "tile_size": 9, "min_level": 8, "max_level": 12,
            "shard_level": 10, "region": [10.5, 60.5, 11.0, 61.0], "filter": ")" +
                       filter + R"(",
            "sources": [{"files": ["west.tif", "east.tif"]}]
        })");
        bake(load_bake_config(f.dir / (name + ".json")), {});
        std::ostringstream report;
        const validation_report r = validate(f.dir / name, report);
        CHECK(r.ok());
        if (!r.ok())
        {
            std::fputs(report.str().c_str(), stderr);
        }
    }

    // The maximum filter never puts terrain lower than the source: compare the coarsest level
    // with the surface at its sample positions.
    const pack_file p(f.dir / "split-maximum");
    for (const pack_file::shard_info& shard : p.shards())
    {
        p.for_each_tile(shard, [&](const decoded_tile& t, std::span<const std::byte>) {
            if (t.id.level != 8)
            {
                return;
            }
            for (std::uint32_t j = 0; j < 9; ++j)
            {
                for (std::uint32_t i = 0; i < 9; ++i)
                {
                    const vgl::internal::degree_position pos =
                        vgl::internal::elevation_sample_position(t.id, 9, i, j);
                    const std::size_t k = static_cast<std::size_t>(j) * 9 + i;
                    if (t.q[k] != vgl::internal::elevation_nodata && pos.longitude > 10.45 &&
                        pos.longitude < 11.05 && pos.latitude > 60.45 && pos.latitude < 61.05)
                    {
                        CHECK(t.height(k) >= surface(pos.latitude, pos.longitude) - 1.0);
                    }
                }
            }
        });
    }
}

namespace
{

// Counts the nodata samples of every tile of a pack that lies fully inside a region, and of
// the pole rows of every tile.
std::uint64_t holes(const fs::path& pack, double west, double south, double east, double north)
{
    const pack_file p(pack);
    const std::uint32_t n = p.manifest().layer().tile_size;
    std::uint64_t count = 0;
    for (const pack_file::shard_info& shard : p.shards())
    {
        p.for_each_tile(shard, [&](const decoded_tile& t, std::span<const std::byte>) {
            const vgl::internal::degree_region b = vgl::internal::tile_bounds(t.id);
            const bool inside =
                b.west >= west && b.east <= east && b.south >= south && b.north <= north;
            for (std::uint32_t j = 0; j < n; ++j)
            {
                const bool pole_row =
                    (j == 0 && b.north == 90.0) || (j == n - 1 && b.south == -90.0);
                for (std::uint32_t i = 0; i < n; ++i)
                {
                    if ((inside || pole_row) &&
                        t.q[static_cast<std::size_t>(j) * n + i] == vgl::internal::elevation_nodata)
                    {
                        ++count;
                    }
                }
            }
        });
    }
    return count;
}

} // namespace

TEST_CASE(pole_rows_have_data_at_levels_finer_than_the_source)
{
    // A global source with 5-degree pixels, baked to levels whose spacing is below the pixel
    // size: interpolation kernels and small footprints both miss the rows near the poles.
    // Like GEBCO, its origin is a hair below 90 degrees and it has average overviews.
    const fixture f;
    write_raster(f.dir / "coarse.tif", -180.0, 90.0 - 1e-14, 5.0, 72, 36, surface);
    {
        GDALDatasetH ds = GDALOpen((f.dir / "coarse.tif").string().c_str(), GA_Update);
        int factors[2] = {2, 4};
        REQUIRE(GDALBuildOverviews(ds, "AVERAGE", 2, factors, 0, nullptr, nullptr, nullptr) ==
                CE_None);
        GDALClose(ds);
    }
    write_text(f.dir / "coarse.json", R"({
        "output": "coarse", "tile_size": 9, "min_level": 0, "max_level": 4, "shard_level": 5,
        "sources": [{"files": "coarse.tif"}]
    })");
    bake(load_bake_config(f.dir / "coarse.json"), {});
    CHECK_EQ(holes(f.dir / "coarse", 0.0, 0.0, 0.0, 0.0), 0u);
    std::ostringstream report;
    CHECK(validate(f.dir / "coarse", report).ok());
}

TEST_CASE(filtered_levels_have_no_holes_with_wide_pixels)
{
    // Pixels three times wider than tall, like GLO-30 above 70 degrees latitude. At level 12
    // with 17-sample tiles, the spacing (0.0027 degrees) is above 1.5 times the pixel height
    // but below the pixel width: filtering there would leave footprints without pixels.
    const fixture f;
    write_raster_cells(f.dir / "wide.tif", 10.4, 61.1, 0.003, 0.001, 233, 700, surface);
    write_text(f.dir / "wide.json", R"({
        "output": "wide", "tile_size": 17, "min_level": 10, "max_level": 12, "shard_level": 11,
        "region": [10.5, 60.5, 11.0, 61.0], "sources": [{"files": "wide.tif"}]
    })");
    bake(load_bake_config(f.dir / "wide.json"), {});
    CHECK_EQ(holes(f.dir / "wide", 10.45, 60.45, 11.05, 61.05), 0u);
}

TEST_CASE(shard_level_defaults_to_five_levels_above_the_deepest)
{
    const fixture f;
    write_text(f.dir / "defaults.json",
               R"({"output": "x", "max_level": 12, "sources": [{"files": "terrain.tif"}]})");
    CHECK_EQ(load_bake_config(f.dir / "defaults.json").shard_level, 7u);
    write_text(f.dir / "shallow.json",
               R"({"output": "x", "max_level": 3, "sources": [{"files": "terrain.tif"}]})");
    CHECK_EQ(load_bake_config(f.dir / "shallow.json").shard_level, 0u);
}

TEST_CASE(sea_blends_into_the_next_source_at_the_edge)
{
    // Land west of 10.8 E and sea, exactly 0 m, east of it to the edge of the region at 11 E;
    // under it a seabed 80 m below the geoid, as GEBCO is under GLO-30.
    const fixture f;
    write_raster(f.dir / "coast.tif", 10.4, 61.1, 0.001, 700, 700, [](double la, double lo) {
        return lo < 10.8 ? surface(la, lo) : 0.0;
    });
    write_raster(
        f.dir / "seabed.tif", 10.0, 61.5, 0.01, 200, 150, [](double, double) { return -80.0; });
    const auto bake_coast = [&](const std::string& output, const std::string& blend) {
        write_text(f.dir / (output + ".json"),
                   R"({
            "output": ")" +
                       output + R"(", "tile_size": 17, "min_level": 8, "max_level": 13,
            "shard_level": 10, "region": [10.5, 60.5, 11.0, 61.0], "geoid": "geoid",
            "sources": [{"files": "coast.tif", "datum": "egm2008")" +
                       blend + R"(},
                        {"files": "seabed.tif", "datum": "egm2008", "fill_only": true}]
        })");
        bake(load_bake_config(f.dir / (output + ".json")), {});
        return pack_file(f.dir / output);
    };
    const double sea = geoid_height;
    const double seabed = -80.0 + geoid_height;

    // Without the blend, the sea ends at sea level, 80 m above the seabed beyond it.
    {
        const pack_file p = bake_coast("plain", "");
        height_query q(p);
        CHECK_NEAR(q.height(60.75, 10.9995).height, sea, 0.1);
        CHECK_NEAR(q.height(60.75, 10.9).height, sea, 0.1);
    }

    // With it, the sea meets the seabed at the edge and eases to sea level 5 km in. Land and
    // the sea further in stay as they are.
    const pack_file p = bake_coast("blended", R"(, "sea_blend": 5000)");
    height_query q(p);
    CHECK_NEAR(q.height(60.75, 10.9995).height, seabed, 0.5);
    const double near = q.height(60.75, 10.96).height; // About 2.2 km in.
    CHECK(near > seabed + 10.0 && near < sea - 10.0);
    const double further = q.height(60.75, 10.93).height; // About 3.8 km in.
    CHECK(further > near && further < sea);
    CHECK_NEAR(q.height(60.75, 10.85).height, sea, 0.1); // About 8 km in.
    CHECK_NEAR(q.height(60.75, 10.6).height, surface(60.75, 10.6) + geoid_height, 0.1);
    std::ostringstream report;
    CHECK(validate(f.dir / "blended", report).ok());
}

TEST_CASE(configuration_errors_name_the_problem)
{
    const fixture f;
    const auto error_of = [&](const std::string& json) {
        write_text(f.dir / "bad.json", json);
        try
        {
            static_cast<void>(load_bake_config(f.dir / "bad.json"));
        }
        catch (const std::runtime_error& e)
        {
            return std::string(e.what());
        }
        return std::string();
    };
    CHECK(error_of(R"({"sources": []})").find("\"output\" is required") != std::string::npos);
    CHECK(error_of(R"({"output": "x", "tile_size": 128, "sources": [{"files": "terrain.tif"}]})")
              .find("tile_size") != std::string::npos);
    CHECK(error_of(R"({"output": "x", "sources": [{"files": "terrain.tif", "datum": "egm2008"}]})")
              .find("geoid") != std::string::npos);
    CHECK(error_of(R"({"output": "x", "sources": [{"files": "missing-*.tif"}]})")
              .find("matches no files") != std::string::npos);
    CHECK(error_of(R"({"output": "x", "sources": [{"files": "terrain.tif", "sea_blend": -1}]})")
              .find("sea_blend") != std::string::npos);
}

// ------------------------------------------------------------------------------------------
// Imagery
// ------------------------------------------------------------------------------------------

namespace
{

// The synthetic imagery: smooth colours that vary with latitude and longitude in degrees.
std::uint8_t colour(double latitude, double longitude, int band)
{
    constexpr double radians = 3.14159265358979323846 / 180.0;
    double v = 0.0;
    switch (band)
    {
    case 0:
        v = 128.0 + 100.0 * std::sin(longitude * radians);
        break;
    case 1:
        v = 128.0 + 100.0 * std::cos(2.0 * latitude * radians);
        break;
    default:
        v = 128.0 + 0.6 * latitude + 0.2 * longitude;
        break;
    }
    return static_cast<std::uint8_t>(std::clamp(std::round(v), 0.0, 255.0));
}

// Writes an 8-bit GeoTIFF with square pixels, whose pixel centres sample colour().
void write_image(
    const fs::path& path, double west, double north, double pixel, int width, int height, int bands)
{
    GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"),
                                 path.string().c_str(),
                                 width,
                                 height,
                                 bands,
                                 GDT_Byte,
                                 nullptr);
    double gt[6] = {west, pixel, 0.0, north, 0.0, -pixel};
    GDALSetGeoTransform(ds, gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 4326);
    GDALSetSpatialRef(ds, srs);
    OSRDestroySpatialReference(srs);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width));
    for (int b = 0; b < bands; ++b)
    {
        GDALRasterBandH band = GDALGetRasterBand(ds, b + 1);
        for (int j = 0; j < height; ++j)
        {
            for (int i = 0; i < width; ++i)
            {
                row[static_cast<std::size_t>(i)] =
                    colour(north - (j + 0.5) * pixel, west + (i + 0.5) * pixel, b);
            }
            if (GDALRasterIO(
                    band, GF_Write, 0, j, width, 1, row.data(), width, 1, GDT_Byte, 0, 0) !=
                CE_None)
            {
                throw std::runtime_error("cannot write " + path.string());
            }
        }
    }
    GDALClose(ds);
}

// Every tile of a pack with its decoded image.
std::map<std::uint64_t, std::pair<pack::tile_header, image>> imagery_tiles(const fs::path& pack)
{
    std::map<std::uint64_t, std::pair<pack::tile_header, image>> tiles;
    const pack_file p(pack);
    for (const pack_file::shard_info& shard : p.shards())
    {
        p.for_each_tile(shard, [&](const decoded_tile& t, std::span<const std::byte> bytes) {
            tiles[vgl::internal::tile_key(t.id)] = {
                t.header, decode_image(bytes.subspan(t.header.header_size))};
        });
    }
    return tiles;
}

} // namespace

TEST_CASE(baked_imagery_matches_the_source_with_borders)
{
    // A global RGB image of 0.25 degree pixels. With 64-pixel tiles, levels 0 to 2 filter it
    // (0.7 degree pixels and coarser) and level 3 interpolates it (0.35 degrees).
    const fixture f;
    write_image(f.dir / "world.tif", -180.0, 90.0, 0.25, 1440, 720, 3);
    const std::string config = R"(
        "layer": "imagery", "tile_size": 64, "max_level": 3, "shard_level": 2,
        "metadata": {"attribution": "Synthetic imagery"}, "sources": [{"files": "world.tif"}]
    })";
    write_text(f.dir / "imagery.json", R"({"output": "imagery",)" + config);
    const bake_config c = load_bake_config(f.dir / "imagery.json");
    CHECK_EQ(c.border, 1u);
    bake(c, {});

    const pack_file p(f.dir / "imagery");
    const pack::layer_description& l = p.manifest().layer();
    CHECK(l.type == pack::layer_type::imagery);
    CHECK_EQ(l.tile_size, static_cast<std::uint16_t>(64));
    CHECK_EQ(l.border, static_cast<std::uint8_t>(1));
    CHECK_EQ(l.height_reference, static_cast<std::uint8_t>(0));

    const auto tiles = imagery_tiles(f.dir / "imagery");
    CHECK_EQ(tiles.size(), static_cast<std::size_t>(2 + 8 + 32 + 128));
    double worst = 0.0;
    for (const auto& [key, tile] : tiles)
    {
        const tile_id t = vgl::internal::tile_from_key(key);
        const auto& [header, img] = tile;
        // Complete tiles are JPEG, RGB, with the border: 66 x 66 pixels.
        REQUIRE(header.encoding == static_cast<std::uint8_t>(pack::imagery_encoding::jpeg));
        REQUIRE(img.width == 66u && img.height == 66u && img.channels == 3u);
        CHECK_EQ(header.child_mask, static_cast<std::uint8_t>(t.level < 3 ? 0xF : 0));
        // Inner pixels hold the colour at their centre, to within JPEG's losses.
        if (t.level == 3)
        {
            const vgl::internal::degree_region b = vgl::internal::tile_bounds(t);
            const double step = vgl::internal::tile_span(3) / 64.0;
            for (std::uint32_t j = 8; j < 58; j += 7)
            {
                for (std::uint32_t i = 8; i < 58; i += 7)
                {
                    const double lat = b.north - (j - 1 + 0.5) * step;
                    const double lon = b.west + (i - 1 + 0.5) * step;
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        const int got = img.pixels[(static_cast<std::size_t>(j) * 66 + i) * 3 +
                                                   static_cast<std::size_t>(ch)];
                        worst = std::max(
                            worst, std::fabs(got - static_cast<double>(colour(lat, lon, ch))));
                    }
                }
            }
        }
    }
    std::printf("synthetic imagery: largest colour error %.0f of 255\n", worst);
    CHECK(worst <= 12.0);

    std::ostringstream report;
    const validation_report r = validate(f.dir / "imagery", report);
    CHECK(r.ok());
    if (!r.ok())
    {
        std::fputs(report.str().c_str(), stderr);
    }

    // Imagery bakes are deterministic too.
    write_text(f.dir / "again.json", R"({"output": "again",)" + config);
    bake_options parallel;
    parallel.jobs = 3;
    bake(load_bake_config(f.dir / "again.json"), parallel);
    CHECK(pack_files(f.dir / "imagery") == pack_files(f.dir / "again"));
}

TEST_CASE(partial_imagery_tiles_are_png_with_alpha)
{
    // A regional grey image: tiles on its edges have pixels without data.
    const fixture f;
    write_image(f.dir / "region.tif", 10.0, 55.0, 0.05, 300, 300, 1);
    write_text(f.dir / "region.json", R"({
        "output": "region", "layer": "imagery", "tile_size": 64, "min_level": 3,
        "max_level": 5, "shard_level": 4, "sources": [{"files": "region.tif"}]
    })");
    bake(load_bake_config(f.dir / "region.json"), {});

    std::size_t png = 0;
    std::size_t jpeg = 0;
    for (const auto& [key, tile] : imagery_tiles(f.dir / "region"))
    {
        const auto& [header, img] = tile;
        if (header.encoding == static_cast<std::uint8_t>(pack::imagery_encoding::png))
        {
            ++png;
            REQUIRE(img.channels == 4u);
            // Some pixels have no data, and those are transparent black.
            bool transparent = false;
            for (std::size_t k = 0; k < img.pixels.size(); k += 4)
            {
                if (img.pixels[k + 3] == 0)
                {
                    transparent = true;
                    CHECK(img.pixels[k] == 0 && img.pixels[k + 1] == 0 && img.pixels[k + 2] == 0);
                }
            }
            CHECK(transparent);
        }
        else
        {
            ++jpeg;
            CHECK_EQ(img.channels, 1u);
        }
    }
    CHECK(png > 0);
    CHECK(jpeg > 0);

    std::ostringstream report;
    const validation_report r = validate(f.dir / "region", report);
    CHECK(r.ok());
    if (!r.ok())
    {
        std::fputs(report.str().c_str(), stderr);
    }
}

TEST_CASE(imagery_configuration_errors_name_the_problem)
{
    const fixture f;
    write_image(f.dir / "rgba.tif", 10.0, 50.0, 0.05, 20, 20, 4);
    const auto error_of = [&](const std::string& json, bool run) {
        write_text(f.dir / "bad.json", json);
        try
        {
            const bake_config c = load_bake_config(f.dir / "bad.json");
            if (run)
            {
                bake(c, {});
            }
        }
        catch (const std::runtime_error& e)
        {
            return std::string(e.what());
        }
        return std::string();
    };
    const std::string head = R"({"layer": "imagery", "output": ")";
    const std::string terrain = R"("sources": [{"files": "terrain.tif"}]})";
    CHECK(error_of(head + R"(x", "tile_size": 257, )" + terrain, false).find("power of two") !=
          std::string::npos);
    CHECK(error_of(head + R"(x", "border": 2, )" + terrain, false).find("border") !=
          std::string::npos);
    CHECK(error_of(head + R"(x", "encoding": "webp", )" + terrain, false).find("encoding") !=
          std::string::npos);
    CHECK(error_of(head + R"(x", "geoid": "geoid", )" + terrain, false)
              .find("apply only to heights") != std::string::npos);
    CHECK(error_of(R"({"output": "x", "border": 1, )" + terrain, false)
              .find("applies only to imagery") != std::string::npos);
    // The terrain is 32-bit floating point, and the RGBA image has four bands.
    CHECK(error_of(head + R"(y", "max_level": 1, )" + terrain, true).find("8-bit") !=
          std::string::npos);
    CHECK(error_of(head + R"(z", "max_level": 1, "sources": [{"files": "rgba.tif"}]})", true)
              .find("1 (grey) or 3 (RGB) bands") != std::string::npos);
}
