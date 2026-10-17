// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Headless sample (design document, section 11): flies a scripted camera over the globe,
// loading and selecting tiles and taking frames without a GPU, and prints the statistics of
// each view. It uses only the public API, with the defaults module's thread pool and file
// reader. With --check, it also verifies what it sees and exits with 1 on a failure, so that
// it doubles as a test.
//
//   vgl-headless [--data <directory>] [--check]
//
// The directory holds packs/gebco, packs/egm2008 and packs/bluemarble (samples/data).

#include "sample_camera.hpp"

#include <vgl/defaults.hpp>
#include <vgl/vgl.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef VGL_SAMPLE_DATA_DIR
#    define VGL_SAMPLE_DATA_DIR "."
#endif

namespace
{

int failures = 0;

void expect(bool condition, const char* what)
{
    if (!condition)
    {
        std::fprintf(stderr, "check failed: %s\n", what);
        ++failures;
    }
}

struct waypoint
{
    const char* name;
    double latitude;  // Degrees.
    double longitude; // Degrees.
    double altitude;  // Metres above the ellipsoid.
    double pitch;     // Degrees below the horizon; 90 looks straight down.
};

vgl_view make_view(const waypoint& w)
{
    // Looking north, tilted down by the pitch.
    const sample::camera c{w.latitude, w.longitude, w.altitude, 0.0, w.pitch};
    auto view = vgl::init<vgl_view>();
    view.view_matrix = sample::camera_view_matrix(c);
    view.projection_matrix =
        sample::perspective(vgl::degrees_to_radians(60.0), 16.0 / 9.0, 1.0, 1e9);
    view.viewport_width = 1920;
    view.viewport_height = 1080;
    return view;
}

// A consumer without a GPU: it keeps the sizes of the resources it "creates".
class consumer
{
public:
    void apply(const vgl::frame& f)
    {
        for (const std::uint64_t id : f.destroys())
        {
            bytes_ -= sizes_[id];
            sizes_.erase(id);
        }
        for (const vgl_index_list_data& d : f.index_lists())
        {
            add(d.resource, std::uint64_t{d.index_count} * d.index_size);
        }
        for (const vgl_mesh_data& d : f.meshes())
        {
            add(d.resource,
                std::uint64_t{d.vertex_count} * d.vertex_size +
                    std::uint64_t{d.sample_grid} * d.sample_grid * d.sample_size);
        }
        for (const vgl_texture_data& d : f.textures())
        {
            std::uint64_t size = 0;
            for (std::uint32_t level = 0; level < d.mip_count; ++level)
            {
                size += d.mip_sizes[level];
            }
            add(d.resource, size);
        }
        drawn_ = f.draw_list().size();
        layered_ = 0;
        for (const vgl_draw_item& item : f.draw_list())
        {
            layered_ += item.layer_count != 0 ? 1 : 0;
        }
    }

    [[nodiscard]] std::uint64_t bytes() const { return bytes_; }
    [[nodiscard]] std::size_t drawn() const { return drawn_; }
    // Drawn tiles with imagery.
    [[nodiscard]] std::size_t layered() const { return layered_; }

private:
    void add(std::uint64_t id, std::uint64_t size)
    {
        sizes_[id] = size;
        bytes_ += size;
    }

    std::unordered_map<std::uint64_t, std::uint64_t> sizes_;
    std::uint64_t bytes_ = 0;
    std::size_t drawn_ = 0;
    std::size_t layered_ = 0;
};

double mib(std::uint64_t bytes)
{
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

} // namespace

int main(int argc, char** argv)
{
    std::string data = VGL_SAMPLE_DATA_DIR;
    bool check = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--check") == 0)
        {
            check = true;
        }
        else if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc)
        {
            data = argv[++i];
        }
        else
        {
            std::fprintf(stderr, "usage: vgl-headless [--data <directory>] [--check]\n");
            return 2;
        }
    }

    // The defaults: a thread pool for decoding and reading, and a file reader on it.
    vgl::expected<vgl::thread_pool> pool = vgl::thread_pool::create();
    if (!pool)
    {
        std::fprintf(
            stderr, "could not create the thread pool: %s\n", vgl::result_name_get(pool.error()));
        return 1;
    }
    vgl::expected<vgl::file_reader> reader = vgl::file_reader::create(*pool);
    if (!reader)
    {
        std::fprintf(
            stderr, "could not create the file reader: %s\n", vgl::result_name_get(reader.error()));
        return 1;
    }
    const vgl_task_interface tasks = pool->task_interface_get();
    const vgl_io_interface io = reader->io_interface_get();

    auto info = vgl::init<vgl_context_create_info>();
    info.tasks = &tasks;
    info.io = &io;
    info.log = [](void*, vgl_log_level, const char* message) {
        std::fprintf(stderr, "vgl: %s\n", message);
    };
    vgl::expected<vgl::context> ctx = vgl::context::create(&info);
    if (!ctx)
    {
        std::fprintf(
            stderr, "could not create the context: %s\n", vgl::result_name_get(ctx.error()));
        return 1;
    }
    const std::string terrain_uri = data + "/packs/gebco";
    const std::string geoid_uri = data + "/packs/egm2008";
    const std::string imagery_uri = data + "/packs/bluemarble";
    const std::string detail_uri = data + "/packs/glo30-sognefjord";
    vgl::expected<vgl::pack> terrain = vgl::pack::create(*ctx, terrain_uri.c_str());
    // Finer terrain over the Sognefjord, above the global GEBCO.
    vgl::expected<vgl::pack> detail = vgl::pack::create(*ctx, detail_uri.c_str(), 1);
    vgl::expected<vgl::pack> geoid = vgl::pack::create(*ctx, geoid_uri.c_str());
    vgl::expected<vgl::pack> imagery = vgl::pack::create(*ctx, imagery_uri.c_str());
    if (!terrain || !geoid || !imagery || !detail)
    {
        std::fprintf(stderr, "could not add the packs\n");
        return 1;
    }

    const waypoint path[] = {
        {"orbit, Atlantic", 30.0, -30.0, 2.0e7, 90.0},
        {"5000 km, Europe", 48.0, 10.0, 5.0e6, 90.0},
        {"1000 km, Alps", 46.5, 9.0, 1.0e6, 90.0},
        {"100 km, Alps", 46.5, 9.0, 1.0e5, 60.0},
        {"10 km, Himalaya", 27.0, 86.9, 1.0e4, 20.0},
        {"20 km, Sognefjord", 61.2, 7.0, 2.0e4, 45.0},
        {"orbit, Pacific", 0.0, -160.0, 2.0e7, 90.0},
    };

    consumer gpu;
    std::printf("%-17s %6s %8s %8s %8s %6s %6s %7s %8s %8s %8s\n",
                "view",
                "frames",
                "upd ms",
                "frm ms",
                "selected",
                "drawn",
                "reads",
                "loaded",
                "load ms",
                "cpu MiB",
                "gpu MiB");
    for (const waypoint& w : path)
    {
        const vgl_view view = make_view(w);
        expect(ctx->view_set(&view) == vgl::result::success, "the view is valid");
        const vgl_context_statistics before = ctx->statistics_get();
        int frames = 0;
        int quiet = 0;
        double update_ms = 0.0;
        double frame_ms = 0.0;
        // Frames until nothing has happened for a few in a row: nothing read, decoded, built or
        // created, and everything selected drawn.
        while (frames < 3000 && quiet < 5)
        {
            expect(ctx->update() == vgl::result::success, "vgl_context_update succeeds");
            {
                vgl::expected<vgl::frame> f = vgl::frame::begin(*ctx);
                expect(f.has_value(), "vgl_context_frame_begin succeeds");
                if (f)
                {
                    gpu.apply(*f);
                }
            }
            const vgl_context_statistics s = ctx->statistics_get();
            update_ms += s.update_time * 1e3;
            frame_ms += s.frame_time * 1e3;
            ++frames;
            const bool idle = s.packs_loading == 0 && s.reads_outstanding == 0 &&
                              s.tiles_decoding == 0 && s.meshes_building == 0 &&
                              s.meshes_waiting == 0 && s.textures_waiting == 0 &&
                              s.resources_created == 0 && s.tiles_drawn == s.tiles_selected;
            quiet = idle ? quiet + 1 : 0;
            if (!idle)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Let the pool work.
            }
        }
        const vgl_context_statistics s = ctx->statistics_get();
        std::printf(
            "%-17s %6d %8.3f %8.3f %8u %6u %6llu %7llu %8.2f %8.1f %8.1f\n",
            w.name,
            frames,
            update_ms / frames,
            frame_ms / frames,
            s.tiles_selected,
            s.tiles_drawn,
            static_cast<unsigned long long>(s.reads_total - before.reads_total),
            static_cast<unsigned long long>(s.tiles_loaded_total - before.tiles_loaded_total),
            s.tile_load_time_mean * 1e3,
            mib(s.cpu_used),
            mib(s.gpu_used));

        expect(quiet >= 5, "loading settles");
        expect(s.packs_failed == 0, "no pack fails");
        expect(s.tiles_selected > 0, "tiles are selected");
        expect(s.tiles_drawn == s.tiles_selected, "every selected tile is drawn");
        expect(gpu.layered() == gpu.drawn(), "every drawn tile has imagery");
        expect(s.gpu_used == gpu.bytes(), "the consumer holds what the core counts");
        expect(s.gpu_used <= s.gpu_budget, "the GPU budget holds");
        expect(s.cpu_used <= s.cpu_budget, "the CPU budget holds");
        // GEBCO covers every view, so its credit must be shown in every view.
        const std::vector<const char*> lines = ctx->attribution_get();
        expect(std::any_of(lines.begin(),
                           lines.end(),
                           [](const char* line) { return std::strstr(line, "GEBCO") != nullptr; }),
               "GEBCO is credited");
        expect(std::any_of(lines.begin(),
                           lines.end(),
                           [](const char* line) { return std::strstr(line, "NASA") != nullptr; }),
               "Blue Marble is credited");
    }

    // The credits a host would display with the last view.
    std::printf("\ncredits:\n");
    for (const char* line : ctx->attribution_get())
    {
        std::printf("  %s\n", line);
    }

    // Heights above mean sea level at a few well-known places. At this sample's resolution of
    // about 20 km they are smoothed, so the checks are loose.
    struct place
    {
        const char* name;
        double latitude;
        double longitude;
        double lowest;
        double highest;
    };
    const place places[] = {
        {"Mount Everest", 27.988, 86.925, 2000.0, 8849.0},
        {"Challenger Deep", 11.35, 142.2, -11000.0, -4000.0},
        {"Mid-Atlantic, 30 N 40 W", 30.0, -40.0, -6000.0, -1000.0},
        {"Netherlands, 52 N 5 E", 52.0, 5.0, -100.0, 200.0},
        // From the GLO-30 detail, 76 m between samples: close to the summit's 2469 m.
        {"Galdhopiggen (GLO-30)", 61.6363, 8.3125, 2300.0, 2469.0},
    };
    std::printf("\n%-25s %12s %12s\n", "place", "ellipsoid m", "sea level m");
    for (const place& p : places)
    {
        const double lat = vgl::degrees_to_radians(p.latitude);
        const double lon = vgl::degrees_to_radians(p.longitude);
        vgl::expected<vgl::height_sample> h_e = ctx->height_query(lat, lon);
        vgl::expected<vgl::height_sample> h_m =
            ctx->height_query(lat, lon, vgl::height_reference::mean_sea_level);
        // Queries start the reads they need; finish them.
        for (int i = 0;
             i < 3000 && h_e && h_m &&
             (h_e->status != vgl::result::success || h_m->status != vgl::result::success);
             ++i)
        {
            static_cast<void>(ctx->update());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            h_e = ctx->height_query(lat, lon);
            h_m = ctx->height_query(lat, lon, vgl::height_reference::mean_sea_level);
        }
        const bool ok = h_e && h_m && h_e->status == vgl::result::success &&
                        h_m->status == vgl::result::success;
        expect(ok, "height queries succeed");
        if (ok)
        {
            std::printf("%-25s %12.1f %12.1f\n", p.name, h_e->height, h_m->height);
            expect(h_m->height >= p.lowest && h_m->height <= p.highest, p.name);
            expect(std::fabs((h_e->height - h_m->height)) < 110.0,
                   "the geoid separation is plausible");
        }
    }

    // Leave in order: packs and context, then the reader, then the pool.
    terrain->reset();
    geoid->reset();
    imagery->reset();
    detail->reset();
    ctx->reset();
    reader->reset();
    pool->reset();

    if (check)
    {
        std::printf("\n%s\n", failures == 0 ? "all checks passed" : "checks failed");
        return failures == 0 ? 0 : 1;
    }
    return 0;
}
