// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "baker.hpp"

#include "decoders/elevation_codec.hpp"
#include "image_codec.hpp"
#include "pack_file.hpp"
#include "pack_writer.hpp"
#include "source.hpp"

#include <vgl/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace vgl::bake
{

namespace
{

using vgl::internal::tile_bounds;
using vgl::internal::tile_child;
using vgl::internal::tile_columns;
using vgl::internal::tile_key;
using vgl::internal::tile_rows;
using vgl::internal::tile_span;

constexpr double nan = std::numeric_limits<double>::quiet_NaN();
// Metres per degree along a meridian, near enough for distances of kilometres.
constexpr double metres_per_degree = 111320.0;

// What a parent needs to know about a child (docs/pack-format.md, section 6.2).
struct tile_stats
{
    bool exists = false;
    double min = 0.0;
    double max = 0.0;
};

// Record of a finished shard, so that a bake can resume.
struct shard_record
{
    bool empty = true;
    std::uint32_t digest = 0;
    std::uint64_t file_size = 0;
    std::uint64_t tile_count = 0;
    tile_stats root; // Stats of the shard's root tile, for the base shard.
};

bool intersects(const degree_region& a, const degree_region& b) noexcept
{
    return a.west < b.east && b.west < a.east && a.south < b.north && b.south < a.north;
}

// Rounds a value to float, away from the inside of [min, max], so float bounds stay conservative.
float round_down(double v)
{
    const auto f = static_cast<float>(v);
    return static_cast<double>(f) > v ? std::nextafter(f, -std::numeric_limits<float>::infinity())
                                      : f;
}

float round_up(double v)
{
    const auto f = static_cast<float>(v);
    return static_cast<double>(f) < v ? std::nextafter(f, std::numeric_limits<float>::infinity())
                                      : f;
}

void write_file_atomically(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
    std::filesystem::create_directories(path.parent_path());
    const std::filesystem::path temporary = path.string() + ".partial";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            throw std::runtime_error("cannot write " + temporary.string());
        }
    }
    std::filesystem::rename(temporary, path);
}

std::string record_text(const shard_record& r)
{
    std::ostringstream os;
    os.precision(17);
    os << (r.empty ? 1 : 0) << ' ' << r.digest << ' ' << r.file_size << ' ' << r.tile_count << ' '
       << (r.root.exists ? 1 : 0) << ' ' << r.root.min << ' ' << r.root.max << '\n';
    return os.str();
}

bool parse_record(const std::filesystem::path& path, shard_record& r)
{
    std::ifstream in(path);
    int empty = 0;
    int exists = 0;
    if (!(in >> empty >> r.digest >> r.file_size >> r.tile_count >> exists >> r.root.min >>
          r.root.max))
    {
        return false;
    }
    r.empty = empty != 0;
    r.root.exists = exists != 0;
    return true;
}

class pipeline
{
public:
    pipeline(const bake_config& config, const bake_options& options)
        : config_(config), options_(options),
          max_level_(options.max_level ? std::min(*options.max_level, config.max_level)
                                       : config.max_level),
          region_{config.west, config.south, config.east, config.north}
    {
        for (const source_config& s : config_.sources)
        {
            sources_.emplace_back(s);
        }
        if (imagery())
        {
            // Grey sources can join colour ones; the pack is grey only if all of them are.
            for (const source& s : sources_)
            {
                if (!s.byte_data() || (s.bands() != 1 && s.bands() != 3))
                {
                    throw std::runtime_error(
                        "source \"" + s.config().name +
                        "\": imagery sources must have 1 (grey) or 3 (RGB) bands of 8-bit "
                        "values; convert others with gdal_translate (for example -b 1 -b 2 -b 3 "
                        "-ot Byte -scale)");
                }
                channels_ =
                    std::max<std::uint32_t>(channels_, static_cast<std::uint32_t>(s.bands()));
            }
        }
        if (!config_.geoid.empty())
        {
            geoid_ = std::make_unique<geoid_model>(config_.geoid);
        }
    }

    bake_summary run()
    {
        const auto start = std::chrono::steady_clock::now();
        prepare_output();

        // Level-S shards, in Morton order. With S above the deepest level, everything is in
        // the base shard.
        const std::uint32_t s = config_.shard_level;
        std::vector<tile_id> roots;
        if (s <= max_level_)
        {
            const double span = tile_span(s);
            const auto x0 = static_cast<std::uint32_t>(
                std::max(0.0, std::floor((region_.west + 180.0) / span)));
            const auto x1 =
                std::min(tile_columns(s) - 1,
                         static_cast<std::uint32_t>(std::ceil((region_.east + 180.0) / span)));
            const auto y0 = static_cast<std::uint32_t>(
                std::max(0.0, std::floor((90.0 - region_.north) / span)));
            const auto y1 =
                std::min(tile_rows(s) - 1,
                         static_cast<std::uint32_t>(std::ceil((90.0 - region_.south) / span)));
            for (std::uint32_t y = y0; y <= y1; ++y)
            {
                for (std::uint32_t x = x0; x <= x1; ++x)
                {
                    const tile_id t{s, x, y};
                    if (relevant(t))
                    {
                        roots.push_back(t);
                    }
                }
            }
            std::sort(roots.begin(), roots.end(), [](const tile_id& a, const tile_id& b) {
                return tile_key(a) < tile_key(b);
            });
        }

        records_.assign(roots.size(), {});
        run_parallel(roots.size(),
                     [&](sampler& smp, std::size_t i) { records_[i] = bake_shard(smp, roots[i]); });
        for (std::size_t i = 0; i < roots.size(); ++i)
        {
            root_stats_[tile_key(roots[i])] = records_[i].root;
        }

        shard_record base;
        if (s > config_.min_level)
        {
            sampler smp;
            base = bake_base(smp);
        }

        write_manifest(roots, base);

        bake_summary summary;
        for (const shard_record& r : records_)
        {
            if (!r.empty)
            {
                ++summary.shards;
                summary.tiles += r.tile_count;
                summary.bytes += r.file_size;
            }
        }
        if (!base.empty)
        {
            ++summary.shards;
            summary.tiles += base.tile_count;
            summary.bytes += base.file_size;
        }
        summary.seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return summary;
    }

    // For diagnostics: one tile's heights before quantization.
    std::vector<double> heights_of(const tile_id& t)
    {
        sampler smp;
        std::vector<double> heights;
        static_cast<void>(compute_tile(smp, t, heights));
        return heights;
    }

private:
    [[nodiscard]] bool imagery() const noexcept
    {
        return config_.layer == pack::layer_type::imagery;
    }

    [[nodiscard]] std::filesystem::path record_path(const std::string& name) const
    {
        return config_.output / ".bake" / (name + ".txt");
    }

    void prepare_output() const
    {
        const bool exists = std::filesystem::exists(config_.output / pack::manifest_file_name) ||
                            std::filesystem::exists(config_.output / ".bake");
        if (exists && !options_.resume)
        {
            throw std::runtime_error(
                config_.output.string() +
                " already holds a bake; pass --resume to continue it, or remove the directory");
        }
        std::filesystem::create_directories(config_.output / ".bake");

        // A resumed bake keeps finished shards, so it must make them the same way: with the
        // same configuration and deepest level as the bake it continues.
        const std::string settings =
            config_.settings + "\nmax_level " + std::to_string(max_level_) + "\n";
        const std::filesystem::path settings_path = config_.output / ".bake" / "settings.txt";
        if (options_.resume && std::filesystem::exists(settings_path))
        {
            std::ifstream in(settings_path, std::ios::binary);
            const std::string previous((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            if (previous != settings)
            {
                throw std::runtime_error(
                    config_.output.string() +
                    " holds a bake with another configuration or --max-level; --resume needs "
                    "the same ones, or remove the directory to bake again");
            }
        }
        write_file_atomically(settings_path,
                              std::as_bytes(std::span(settings.data(), settings.size())));
    }

    // True if a tile may hold data: inside the region and covered by a source for its level,
    // other than one that only fills the others' gaps.
    [[nodiscard]] bool relevant(const tile_id& t) const
    {
        const degree_region b = tile_bounds(t);
        if (!intersects(b, region_))
        {
            return false;
        }
        return std::any_of(sources_.begin(), sources_.end(), [&](const source& s) {
            return !s.config().fill_only && t.level <= s.config().max_level && s.covers(b);
        });
    }

    template <typename Work>
    void run_parallel(std::size_t count, Work work)
    {
        const unsigned threads =
            std::max(1u,
                     std::min<unsigned>(options_.jobs != 0 ? options_.jobs
                                                           : std::thread::hardware_concurrency(),
                                        static_cast<unsigned>(std::max<std::size_t>(count, 1))));
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
        std::mutex error_mutex;
        std::string error;
        std::vector<std::thread> workers;
        for (unsigned w = 0; w < threads; ++w)
        {
            workers.emplace_back([&] {
                sampler smp;
                for (std::size_t i = next++; i < count; i = next++)
                {
                    {
                        const std::lock_guard<std::mutex> lock(error_mutex);
                        if (!error.empty())
                        {
                            return;
                        }
                    }
                    try
                    {
                        work(smp, i);
                    }
                    catch (const std::exception& e)
                    {
                        const std::lock_guard<std::mutex> lock(error_mutex);
                        if (error.empty())
                        {
                            error = e.what();
                        }
                        return;
                    }
                    const std::size_t d = ++done;
                    if (!options_.verbose && (d % 16 == 0 || d == count))
                    {
                        std::fprintf(stderr, "\r  %zu of %zu shards", d, count);
                    }
                }
            });
        }
        for (std::thread& t : workers)
        {
            t.join();
        }
        if (count != 0 && !options_.verbose)
        {
            std::fprintf(stderr, "\n");
        }
        if (!error.empty())
        {
            throw std::runtime_error(error);
        }
    }

    // Samples every source onto a tile, in priority order, with ellipsoidal heights. Returns
    // false if no sample has data.
    bool compute_tile(sampler& smp, const tile_id& t, std::vector<double>& heights)
    {
        const std::uint32_t n = config_.tile_size;
        const std::size_t count = static_cast<std::size_t>(n) * n;
        heights.assign(count, nan);
        const degree_region b = tile_bounds(t);
        const double step = tile_span(t.level) / (n - 1);
        std::size_t missing = count;

        std::vector<double> values;
        std::vector<double> column;
        // Sea samples near the edge of a source with sea_blend, which later sources fill first:
        // how far into the blend each lies, from 0 at the edge to 1 at its full distance, or
        // below 0 for the other samples.
        std::vector<double> blend;
        for (const source& s : sources_)
        {
            if (missing == 0)
            {
                break;
            }
            if (t.level > s.config().max_level || !s.covers(b))
            {
                continue;
            }
            // Near the source's resolution, interpolate; well below it, filter. The decision uses
            // the coarsest pixel dimension, so that every footprint holds pixel centres even
            // where pixels are wider than they are tall (GLO-30 above 50 degrees latitude).
            const bool coarse = step >= 1.5 * s.coarsest_pixel_size();
            std::string resampling = s.config().resampling;
            if (coarse)
            {
                resampling = config_.filter == level_filter::maximum ? "max" : "average";
            }
            smp.sample(s, t, n, resampling, values);

            // Samples that represent the same point get the same value in every tile: the
            // antimeridian is sampled at -180, and each pole at longitude 0.
            if (t.x == tile_columns(t.level) - 1)
            {
                smp.sample_grid(s, -180.0, b.north, step, 1, n, resampling, column);
                for (std::uint32_t j = 0; j < n; ++j)
                {
                    values[static_cast<std::size_t>(j) * n + (n - 1)] = column[j];
                }
            }
            // A level-0 tile touches both poles.
            for (const bool north : {true, false})
            {
                if (t.y != (north ? 0 : tile_rows(t.level) - 1))
                {
                    continue;
                }
                smp.sample_grid(s, 0.0, north ? 90.0 : -90.0, step, 1, 1, resampling, column);
                // An interpolation kernel reaches beyond the pole, where a source has no data.
                // Fall back to the average over a footprint at least two source pixels wide,
                // which always holds a row of pixel centres. Either way the value depends only
                // on the level and the source, so it is the same in every tile of the row.
                if (std::isnan(column[0]))
                {
                    const double footprint = std::max(step, 2.0 * s.coarsest_pixel_size());
                    smp.sample_grid(
                        s, 0.0, north ? 90.0 : -90.0, footprint, 1, 1, "average", column);
                }
                const std::size_t row = north ? 0 : static_cast<std::size_t>(n - 1) * n;
                for (std::uint32_t i = 0; i < n; ++i)
                {
                    values[row + i] = column[0];
                }
            }

            const double sea_blend = s.config().sea_blend;
            const std::vector<degree_region> footprints =
                sea_blend > 0.0 ? footprints_near(s, b, sea_blend) : std::vector<degree_region>{};
            for (std::uint32_t j = 0; j < n; ++j)
            {
                for (std::uint32_t i = 0; i < n; ++i)
                {
                    const std::size_t k = static_cast<std::size_t>(j) * n + i;
                    if (!std::isnan(heights[k]) || std::isnan(values[k]))
                    {
                        continue;
                    }
                    if (sea_blend > 0.0 && values[k] == 0.0)
                    {
                        const vgl::internal::degree_position p =
                            vgl::internal::elevation_sample_position(t, n, i, j);
                        const double d =
                            edge_distance(footprints, p.latitude, p.longitude, sea_blend);
                        if (d < sea_blend)
                        {
                            if (blend.empty())
                            {
                                blend.assign(count, -1.0);
                            }
                            blend[k] = d / sea_blend;
                            continue; // Later sources fill it, then it is eased.
                        }
                    }
                    double v = values[k];
                    if (s.config().datum == vertical_datum::egm2008)
                    {
                        const vgl::internal::degree_position p =
                            vgl::internal::elevation_sample_position(t, n, i, j);
                        v += geoid_->height(p.latitude, p.longitude);
                    }
                    heights[k] = v;
                    --missing;
                }
            }
        }
        // The sea near the edge: from the later sources' heights below sea level at the edge,
        // eased to sea level at the blend's full distance.
        for (std::size_t k = 0; k < blend.size(); ++k)
        {
            if (blend[k] < 0.0)
            {
                continue;
            }
            const auto i = static_cast<std::uint32_t>(k % n);
            const auto j = static_cast<std::uint32_t>(k / n);
            const vgl::internal::degree_position p =
                vgl::internal::elevation_sample_position(t, n, i, j);
            // The blending source's sea level, which is 0 m in its datum.
            double sea = 0.0;
            for (const source& s : sources_)
            {
                if (s.config().sea_blend > 0.0 && s.config().datum == vertical_datum::egm2008)
                {
                    sea = geoid_->height(p.latitude, p.longitude);
                    break;
                }
            }
            const double below = std::isnan(heights[k]) ? sea : std::min(heights[k], sea);
            const double x = blend[k];
            const double w = x * x * (3.0 - 2.0 * x); // Smooth at both ends.
            if (std::isnan(heights[k]))
            {
                --missing;
            }
            heights[k] = below + (sea - below) * w;
        }
        return missing != count;
    }

    // The footprints of a source's files near a tile, clipped to the bake's region: where the
    // pack's data from the source can be within a distance in metres of the tile.
    [[nodiscard]] std::vector<degree_region>
    footprints_near(const source& s, const degree_region& b, double distance) const
    {
        const double margin_lat = distance / metres_per_degree;
        const double widest = std::max(std::fabs(b.south), std::fabs(b.north)) + margin_lat;
        const double margin_lon =
            margin_lat / std::max(std::cos(vgl::internal::radians(std::min(widest, 89.0))), 1e-3);
        const degree_region near{
            b.west - margin_lon, b.south - margin_lat, b.east + margin_lon, b.north + margin_lat};
        std::vector<degree_region> out;
        for (const source_file& f : s.files())
        {
            const degree_region r{std::max(f.bounds.west, region_.west),
                                  std::max(f.bounds.south, region_.south),
                                  std::min(f.bounds.east, region_.east),
                                  std::min(f.bounds.north, region_.north)};
            if (r.west < r.east && r.south < r.north && intersects(r, near))
            {
                out.push_back(r);
            }
        }
        return out;
    }

    // The distance in metres from a position to the nearest place outside the footprints, at
    // most limit: for a union of rectangles, the nearest such place lies just beyond one of
    // their edges, square to it, or just beyond one of their corners.
    [[nodiscard]] static double edge_distance(const std::vector<degree_region>& footprints,
                                              double lat,
                                              double lon,
                                              double limit)
    {
        const auto covered = [&](double la, double lo) {
            return std::any_of(footprints.begin(), footprints.end(), [&](const degree_region& r) {
                return la >= r.south && la <= r.north && lo >= r.west && lo <= r.east;
            });
        };
        if (!covered(lat, lon))
        {
            return 0.0;
        }
        const double ky = metres_per_degree;
        const double kx = metres_per_degree * std::cos(vgl::internal::radians(lat));
        constexpr double nudge = 1e-9; // Degrees: just beyond an edge.
        double best = limit;
        const auto consider = [&](double la, double lo) {
            const double d = std::hypot((lo - lon) * kx, (la - lat) * ky);
            if (d < best && !covered(la, lo))
            {
                best = d;
            }
        };
        for (const degree_region& r : footprints)
        {
            const double clat = std::clamp(lat, r.south, r.north);
            const double clon = std::clamp(lon, r.west, r.east);
            consider(clat, r.west - nudge);
            consider(clat, r.east + nudge);
            consider(r.south - nudge, clon);
            consider(r.north + nudge, clon);
            consider(r.south - nudge, r.west - nudge);
            consider(r.south - nudge, r.east + nudge);
            consider(r.north + nudge, r.west - nudge);
            consider(r.north + nudge, r.east + nudge);
        }
        return best;
    }

    // Samples an imagery tile: (P + 2b)^2 pixels (docs/pack-format.md, section 2.4), each from
    // the first source with data there, as 0 to 255 per channel. valid marks the pixels with
    // data. Returns false if no pixel of the tile itself (the border aside) has data.
    bool compute_image(sampler& smp,
                       const tile_id& t,
                       std::vector<std::vector<double>>& channels,
                       std::vector<bool>& valid)
    {
        const std::uint32_t p = config_.tile_size;
        const std::uint32_t b = config_.border;
        const std::uint32_t n = p + 2 * b;
        const std::size_t count = static_cast<std::size_t>(n) * n;
        const degree_region bounds = tile_bounds(t);
        const double step = tile_span(t.level) / p;
        // Pixel centres: pixel (i, j) covers [west + (i - b) step, west + (i - b + 1) step].
        const double first_lon = bounds.west + (0.5 - b) * step;
        const double first_lat = bounds.north - (0.5 - b) * step;

        channels.assign(channels_, std::vector<double>(count, 0.0));
        valid.assign(count, false);
        std::size_t missing = count;
        std::vector<std::vector<double>> values(channels_);
        std::vector<double> column;
        for (const source& s : sources_)
        {
            if (missing == 0)
            {
                break;
            }
            if (t.level > s.config().max_level || !s.covers(bounds))
            {
                continue;
            }
            const bool coarse = step >= 1.5 * s.coarsest_pixel_size();
            const std::string resampling = coarse ? "average" : s.config().resampling;
            for (std::uint32_t c = 0; c < channels_; ++c)
            {
                const int band = s.bands() == 1 ? 1 : static_cast<int>(c) + 1;
                smp.sample_grid(s, first_lon, first_lat, step, n, n, resampling, values[c], band);
                // A border column beyond the antimeridian repeats the other side of it.
                if (b != 0 && (t.x == 0 || t.x == tile_columns(t.level) - 1))
                {
                    const bool west = t.x == 0;
                    const std::uint32_t i = west ? 0 : n - 1;
                    const double lon = first_lon + i * step + (west ? 360.0 : -360.0);
                    smp.sample_grid(s, lon, first_lat, step, 1, n, resampling, column, band);
                    for (std::uint32_t j = 0; j < n; ++j)
                    {
                        values[c][static_cast<std::size_t>(j) * n + i] = column[j];
                    }
                }
            }
            for (std::size_t k = 0; k < count; ++k)
            {
                if (valid[k] || std::any_of(values.begin(), values.end(), [&](const auto& v) {
                        return std::isnan(v[k]);
                    }))
                {
                    continue;
                }
                for (std::uint32_t c = 0; c < channels_; ++c)
                {
                    channels[c][k] = values[c][k];
                }
                valid[k] = true;
                --missing;
            }
        }

        // A border row beyond a pole has nothing to repeat; it repeats the row inside.
        if (b != 0)
        {
            for (const bool north : {true, false})
            {
                if (t.y != (north ? 0 : tile_rows(t.level) - 1))
                {
                    continue;
                }
                const std::size_t to = north ? 0 : static_cast<std::size_t>(n - 1) * n;
                const std::size_t from = north ? n : static_cast<std::size_t>(n - 2) * n;
                for (std::uint32_t i = 0; i < n; ++i)
                {
                    for (auto& ch : channels)
                    {
                        ch[to + i] = ch[from + i];
                    }
                    valid[to + i] = valid[from + i];
                }
            }
        }

        for (std::uint32_t j = b; j < b + p; ++j)
        {
            for (std::uint32_t i = b; i < b + p; ++i)
            {
                if (valid[static_cast<std::size_t>(j) * n + i])
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Encodes an imagery tile: JPEG where every pixel has data, PNG with alpha elsewhere, unless
    // the configuration asks for one of them throughout.
    void encode_image(const std::vector<std::vector<double>>& channels,
                      const std::vector<bool>& valid,
                      const std::array<tile_stats, 4>& children,
                      std::vector<std::byte>& out) const
    {
        const std::uint32_t n = config_.tile_size + 2 * config_.border;
        const bool complete = std::all_of(valid.begin(), valid.end(), [](bool v) { return v; });
        const bool jpeg = config_.encoding == image_encoding::jpeg ||
                          (config_.encoding == image_encoding::automatic && complete);
        // Alpha comes with colour: PNG has no grey with alpha in this format.
        const bool alpha = !jpeg && !complete;
        image img;
        img.width = n;
        img.height = n;
        img.channels = alpha ? 4 : channels_;
        img.pixels.resize(static_cast<std::size_t>(n) * n * img.channels);
        for (std::size_t k = 0; k < valid.size(); ++k)
        {
            for (std::uint32_t c = 0; c < img.channels; ++c)
            {
                double v = 255.0;
                if (c < 3)
                {
                    v = valid[k] ? channels[channels_ == 1 ? 0 : c][k] : 0.0;
                }
                else if (!valid[k])
                {
                    v = 0.0;
                }
                img.pixels[k * img.channels + c] =
                    static_cast<std::uint8_t>(std::clamp(std::round(v), 0.0, 255.0));
            }
        }
        std::uint8_t child_mask = 0;
        for (std::uint32_t c = 0; c < 4; ++c)
        {
            if (children[c].exists)
            {
                child_mask = static_cast<std::uint8_t>(child_mask | (1u << c));
            }
        }
        out = encode_imagery_tile(jpeg ? pack::imagery_encoding::jpeg : pack::imagery_encoding::png,
                                  child_mask,
                                  jpeg ? encode_jpeg(img, config_.jpeg_quality) : encode_png(img));
    }

    // Quantizes and encodes a tile. Returns its stats.
    tile_stats encode_tile(const tile_id& t,
                           const std::vector<double>& heights,
                           const std::array<tile_stats, 4>& children,
                           std::vector<std::byte>& out) const
    {
        const double step = config_.height_steps[t.level - config_.min_level];
        double lo = std::numeric_limits<double>::infinity();
        double hi = -std::numeric_limits<double>::infinity();
        for (const double v : heights)
        {
            if (!std::isnan(v))
            {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        height_tile tile;
        tile.type = config_.layer;
        tile.size = config_.tile_size;
        tile.scale = step;
        tile.offset = step * std::round((lo + hi) / (2.0 * step));
        tile.q.resize(heights.size());
        std::int32_t qmin = vgl::internal::elevation_max_quantized;
        std::int32_t qmax = -vgl::internal::elevation_max_quantized;
        for (std::size_t k = 0; k < heights.size(); ++k)
        {
            if (std::isnan(heights[k]))
            {
                tile.q[k] = vgl::internal::elevation_nodata;
                continue;
            }
            // Round on the level's global grid of steps, halves upwards, then express the result
            // relative to the offset (a whole number of steps). The decoded height is then the
            // same in every tile that has the sample, whatever its offset (section 6.2):
            // rounding (h - offset) / step directly would round ties differently on either
            // side of zero.
            const double q = std::floor(heights[k] / step + 0.5) - tile.offset / step;
            if (std::fabs(q) > vgl::internal::elevation_max_quantized)
            {
                std::ostringstream os;
                os << "tile " << t.level << '/' << t.x << '/' << t.y << ": heights from " << lo
                   << " m to " << hi << " m do not fit in 65534 steps of " << step
                   << " m; use a larger height step for level " << t.level;
                throw std::runtime_error(os.str());
            }
            tile.q[k] = static_cast<std::int16_t>(q);
            qmin = std::min<std::int32_t>(qmin, tile.q[k]);
            qmax = std::max<std::int32_t>(qmax, tile.q[k]);
        }
        for (std::uint32_t c = 0; c < 4; ++c)
        {
            if (children[c].exists)
            {
                tile.child_mask = static_cast<std::uint8_t>(tile.child_mask | (1u << c));
                tile.child_min[c] = round_down(children[c].min);
                tile.child_max[c] = round_up(children[c].max);
            }
        }
        out = encode_height_tile(tile, config_.zstd_level);
        return {true, tile.offset + step * qmin, tile.offset + step * qmax};
    }

    // Bakes a tile and its descendants down to max_level into tiles. child_stats supplies the
    // stats of children that another job baked (the shard roots, for the base shard).
    // The recursion is at most 29 levels deep, the depth of the quadtree.
    // NOLINTNEXTLINE(misc-no-recursion)
    tile_stats bake_subtree(sampler& smp,
                            const tile_id& t,
                            std::uint32_t stop_level,
                            std::map<std::uint64_t, std::vector<std::byte>>& tiles,
                            std::vector<double>& heights)
    {
        if (t.level > max_level_ || !relevant(t))
        {
            return {};
        }
        std::array<tile_stats, 4> children{};
        if (t.level < max_level_)
        {
            for (std::uint32_t c = 0; c < 4; ++c)
            {
                const tile_id child = tile_child(t, c);
                if (child.level == stop_level)
                {
                    const auto it = root_stats_.find(tile_key(child));
                    children[c] = it != root_stats_.end() ? it->second : tile_stats{};
                }
                else
                {
                    children[c] = bake_subtree(smp, child, stop_level, tiles, heights);
                }
            }
        }
        if (t.level < config_.min_level)
        {
            return {};
        }
        std::vector<std::byte> bytes;
        tile_stats stats;
        if (imagery())
        {
            std::vector<std::vector<double>> channels;
            std::vector<bool> valid;
            if (!compute_image(smp, t, channels, valid))
            {
                return {};
            }
            encode_image(channels, valid, children, bytes);
            stats.exists = true;
        }
        else
        {
            if (!compute_tile(smp, t, heights))
            {
                return {};
            }
            stats = encode_tile(t, heights, children, bytes);
        }
        tiles.emplace(tile_key(t), std::move(bytes));
        return stats;
    }

    shard_record finish_shard(const std::map<std::uint64_t, std::vector<std::byte>>& tiles,
                              const tile_id& root,
                              bool base,
                              const tile_stats& root_stats,
                              const std::string& record_name)
    {
        shard_record r;
        r.root = root_stats;
        if (!tiles.empty())
        {
            shard_writer writer(config_.layer,
                                static_cast<std::uint8_t>(base ? config_.min_level : root.level),
                                root.x,
                                root.y,
                                base);
            for (const auto& [key, bytes] : tiles)
            {
                writer.add(vgl::internal::tile_from_key(key), bytes);
            }
            const shard_writer::output out = writer.finish(config_.zstd_level);
            std::array<char, pack::max_path_size> name{};
            if (base)
            {
                static_cast<void>(pack::base_shard_path(out.digest, name));
            }
            else
            {
                static_cast<void>(pack::shard_path(root.x, root.y, out.digest, name));
            }
            write_file_atomically(config_.output / name.data(), out.bytes);
            r.empty = false;
            r.digest = out.digest;
            r.file_size = out.bytes.size();
            r.tile_count = tiles.size();
        }
        const std::string text = record_text(r);
        write_file_atomically(record_path(record_name),
                              std::as_bytes(std::span(text.data(), text.size())));
        return r;
    }

    shard_record bake_shard(sampler& smp, const tile_id& root)
    {
        const std::string name = "shard-" + std::to_string(root.x) + "-" + std::to_string(root.y);
        shard_record r;
        if (options_.resume && parse_record(record_path(name), r))
        {
            return r;
        }
        const auto start = std::chrono::steady_clock::now();
        std::map<std::uint64_t, std::vector<std::byte>> tiles;
        std::vector<double> heights;
        const tile_stats stats =
            bake_subtree(smp, root, vgl::internal::max_tile_level + 1, tiles, heights);
        r = finish_shard(tiles, root, false, stats, name);
        if (options_.verbose)
        {
            std::fprintf(
                stderr,
                "  shard %u/%u/%u: %zu tiles, %.1f MB, %.1f s\n",
                root.level,
                root.x,
                root.y,
                tiles.size(),
                static_cast<double>(r.file_size) / 1e6,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }
        return r;
    }

    shard_record bake_base(sampler& smp)
    {
        shard_record r;
        if (options_.resume && parse_record(record_path("base"), r))
        {
            return r;
        }
        std::map<std::uint64_t, std::vector<std::byte>> tiles;
        std::vector<double> heights;
        const std::uint32_t level = config_.min_level;
        for (std::uint32_t y = 0; y < tile_rows(level); ++y)
        {
            for (std::uint32_t x = 0; x < tile_columns(level); ++x)
            {
                static_cast<void>(
                    bake_subtree(smp, {level, x, y}, config_.shard_level, tiles, heights));
            }
        }
        return finish_shard(tiles, {level, 0, 0}, true, {}, "base");
    }

    void write_manifest(const std::vector<tile_id>& roots, const shard_record& base)
    {
        manifest_contents m;
        m.layer.type = config_.layer;
        m.layer.tile_size = static_cast<std::uint16_t>(config_.tile_size);
        m.layer.min_level = static_cast<std::uint8_t>(config_.min_level);
        m.layer.max_level = static_cast<std::uint8_t>(max_level_);
        m.layer.shard_level = static_cast<std::uint8_t>(config_.shard_level);
        m.layer.border = static_cast<std::uint8_t>(config_.border);
        m.layer.height_reference = imagery() ? 0 : pack::height_reference_ellipsoid;
        m.layer.semi_major_axis = 6378137.0;
        m.layer.inverse_flattening = 298.257223563;
        m.layer.west = region_.west;
        m.layer.south = region_.south;
        m.layer.east = region_.east;
        m.layer.north = region_.north;
        if (!imagery())
        {
            m.height_steps.assign(config_.height_steps.begin(),
                                  config_.height_steps.begin() +
                                      (max_level_ - config_.min_level + 1));
        }
        m.has_base = !base.empty;
        m.base_file_size = base.file_size;
        m.base_digest = base.digest;
        for (std::size_t i = 0; i < roots.size(); ++i)
        {
            if (!records_[i].empty)
            {
                m.shards.push_back(
                    {roots[i].x, roots[i].y, records_[i].file_size, records_[i].digest});
            }
        }
        m.metadata = config_.metadata;
        m.metadata.emplace_back("generator", std::string("vgl-bake ") + VGL_VERSION_STRING);
        const std::vector<std::byte> bytes = vgl::bake::write_manifest(m, config_.zstd_level);
        write_file_atomically(config_.output / pack::manifest_file_name, bytes);
    }

    bake_config config_;
    bake_options options_;
    std::uint32_t max_level_;
    degree_region region_;
    std::vector<source> sources_;
    std::unique_ptr<geoid_model> geoid_;
    std::uint32_t channels_ = 1; // Imagery: 1 grey or 3 RGB.
    std::vector<shard_record> records_;
    std::map<std::uint64_t, tile_stats> root_stats_;
};

} // namespace

bake_summary bake(const bake_config& config, const bake_options& options)
{
    initialize_gdal(options.gdal_cache_megabytes);
    pipeline p(config, options);
    return p.run();
}

std::vector<double> compute_tile_heights(const bake_config& config,
                                         std::uint32_t level,
                                         std::uint32_t x,
                                         std::uint32_t y)
{
    if (config.layer == pack::layer_type::imagery)
    {
        throw std::runtime_error("sample-tile computes heights; this configuration bakes imagery");
    }
    initialize_gdal(512);
    const bake_options options;
    pipeline p(config, options);
    return p.heights_of({level, x, y});
}

} // namespace vgl::bake
