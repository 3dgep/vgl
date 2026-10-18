// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The interactive samples' overlay, drawn with Dear ImGui: a panel with the context's
// statistics, the credits and the camera, controls for the debug views, and the outline of a
// frozen camera's frustum. It uses Dear ImGui's platform and renderer backends only through the
// hooks a sample gives it, so every sample shares it whatever its window and graphics API.

#ifndef VGL_SAMPLES_OVERLAY_HPP
#define VGL_SAMPLES_OVERLAY_HPP

#include "sample_camera.hpp"
#include "sample_data.hpp"

#include <vgl/vgl.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sample
{

// What the debug controls set.
struct debug_settings
{
    bool panel = true;           // The overlay itself (F1).
    bool wireframe = false;      // F2
    bool level_colors = false;   // F3.
    bool imagery_source = false; // F4.
    bool freeze = false;         // F5: select tiles for a frozen camera.
    bool height_colors = false;  // F6: colours by height instead of imagery.
    bool contours = false;       // F7.
    bool sea = false;            // F8: a sea surface over the ocean floor.
    bool atmosphere = false;     // F9: the sky and haze; the interactive samples start with it.
    bool stars = false;          // Behind the sky; the interactive samples start with them.
    bool clouds = false;         // With the atmosphere; the interactive samples start with them.
    bool contour_auto = true;    // The interval follows the altitude.
    float contour_interval = 500.0f; // Metres.
    bool sunlight = true;            // L.
    // The sun's place: the time of day in hours (UTC) and the day of the year, from 1 on
    // 1 January. The defaults put it over north Africa in early May, late in the morning there.
    float time_of_day = 10.55f;
    int day_of_year = 123;
    int year = 2026;
    // The time of day, day and year follow the system clock (UTC), for the sun and the stars.
    bool current_time = false;
    float max_error = 2.0f; // Screen-space error in pixels.
};

// The debug views' flags, which the OpenGL and Direct3D 11 backends share (vgl_gl_debug_flag,
// vgl_d3d11_debug_flag).
std::uint32_t debug_flags(const debug_settings& d);

// The direction towards the sun along the ECEF axes for the settings' time and day: over the
// longitude where it is noon then, and the latitude of its declination that day. It leaves out
// the equation of time, which moves the sun by up to 4 degrees of longitude.
vgl_dvec3 sun_direction(const debug_settings& d);

// An imagery layer the panel shows, with its opacity and order.
struct layer_control
{
    vgl_pack* pack = nullptr;
    const char* name = "";
};

// The imagery layers among the packs added from sources, in the same order.
std::vector<layer_control> layer_controls(const std::vector<pack_source>& sources,
                                          const std::vector<vgl::pack>& packs);

// What the panel shows besides the statistics.
// A disk cache's statistics (vgl_disk_cache_statistics), for a sample that caches.
struct disk_cache_info
{
    bool enabled = false;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t offline_hits = 0;
    std::uint64_t entries = 0;
    std::uint64_t bytes = 0;
    std::uint64_t budget = 0;
    std::uint64_t evictions = 0;
};

struct panel_info
{
    const char* api = "";    // The graphics API and its version.
    const char* device = ""; // The device that draws.
    const char* depth = "";
    camera live;
    camera frozen;
    std::vector<const char*> credits;
    std::vector<layer_control> layers; // Changed through the panel.
    disk_cache_info cache;             // Shown when enabled.
    // The point the mouse orbits or zooms around, in window coordinates, and how strongly to
    // show its reticle (0 for none).
    double focus_x = 0.0;
    double focus_y = 0.0;
    double focus_strength = 0.0;
};

class overlay
{
public:
    overlay() = default;
    overlay(const overlay&) = delete;
    overlay& operator=(const overlay&) = delete;
    overlay(overlay&&) = delete;
    overlay& operator=(overlay&&) = delete;
    ~overlay();

    // Dear ImGui's platform and renderer backends, as a sample sets them up. init runs with the
    // Dear ImGui context created; new_frame runs before each frame, render draws its data, and
    // shutdown undoes init. With external_frames, as with Ogre's Dear ImGui overlay, the
    // backends own the context and the frames: the context exists before init, new_frame starts
    // the frame, and the renderer ends it.
    struct backend
    {
        bool external_frames = false;
        bool (*init)(void* user_data) = nullptr;
        void (*new_frame)(void* user_data) = nullptr;
        void (*render)(void* user_data) = nullptr;
        void (*shutdown)(void* user_data) = nullptr;
        void* user_data = nullptr;
    };

    // Creates the Dear ImGui context and sets up the backends on it.
    bool init(const backend& b);

    // True while the panel uses the mouse or keyboard, which the camera must then ignore.
    [[nodiscard]] bool wants_mouse() const;
    [[nodiscard]] bool wants_keyboard() const;

    // One frame of the overlay: the panel, the reticle of the point the mouse orbits or zooms
    // around, and the frozen frustum when frozen. The frame took frame_seconds on the CPU. view
    // and projection are the live camera's.
    void frame(const vgl_context_statistics& s,
               const panel_info& info,
               double frame_seconds,
               const vgl_dmat4& view,
               const vgl_dmat4& projection,
               const std::array<vgl_dvec3, 5>& frustum, // Eye, then far corners.
               debug_settings& settings);

    // Draws the overlay into the bound render target.
    void render();

private:
    void panel(const vgl_context_statistics& s, const panel_info& info, debug_settings& d);
    static void layers(const std::vector<layer_control>& layers);
    void frustum_lines(const vgl_dmat4& view,
                       const vgl_dmat4& projection,
                       const std::array<vgl_dvec3, 5>& frustum);
    static void reticle(double x, double y, double strength);

    bool initialized_ = false;
    backend backend_;
    std::array<float, 240> frame_ms_ = {};
    std::size_t next_ = 0;
};

// The eye and the four far corners of a camera's frustum, along its view up to a distance.
std::array<vgl_dvec3, 5>
frustum_of(const camera& c, double field_of_view, double aspect, double distance);

} // namespace sample

#endif // VGL_SAMPLES_OVERLAY_HPP
