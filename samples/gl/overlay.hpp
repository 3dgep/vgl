// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The OpenGL sample's overlay, drawn with Dear ImGui: a panel with the context's statistics,
// the credits and the camera, controls for the debug views, and the outline of a frozen
// camera's frustum.

#ifndef VGL_SAMPLES_GL_OVERLAY_HPP
#define VGL_SAMPLES_GL_OVERLAY_HPP

#include "sample_camera.hpp"

#include <vgl/vgl.hpp>

#include <array>
#include <cstddef>
#include <vector>

struct GLFWwindow;

namespace sample
{

// What the debug controls set.
struct debug_settings
{
    bool panel = true;           // The overlay itself (F1).
    bool wireframe = false;      // F2; desktop OpenGL only.
    bool level_colors = false;   // F3.
    bool imagery_source = false; // F4.
    bool freeze = false;         // F5: select tiles for a frozen camera.
    bool sunlight = true;        // L.
    float max_error = 2.0f;      // Screen-space error in pixels.
};

// What the panel shows besides the statistics.
struct panel_info
{
    const char* gl_version = "";
    const char* gl_renderer = "";
    const char* depth = "";
    bool wireframe_available = true;
    camera live;
    camera frozen;
    std::vector<const char*> credits;
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

    // Sets up Dear ImGui on the window's current OpenGL context. Install the window's own
    // callbacks first: Dear ImGui calls them after its own.
    bool init(GLFWwindow* window, bool es);

    // True while the panel uses the mouse or keyboard, which the camera must then ignore.
    [[nodiscard]] bool wants_mouse() const;
    [[nodiscard]] bool wants_keyboard() const;

    // One frame of the overlay: the panel, and the frozen frustum when frozen. The frame
    // took frame_seconds on the CPU. view and projection are the live camera's.
    void frame(const vgl_context_statistics& s,
               const panel_info& info,
               double frame_seconds,
               const vgl_dmat4& view,
               const vgl_dmat4& projection,
               const std::array<vgl_dvec3, 5>& frustum, // Eye, then far corners.
               debug_settings& settings);

    // Draws the overlay into the bound framebuffer.
    void render();

private:
    void panel(const vgl_context_statistics& s, const panel_info& info, debug_settings& d);
    void frustum_lines(const vgl_dmat4& view,
                       const vgl_dmat4& projection,
                       const std::array<vgl_dvec3, 5>& frustum);

    bool initialized_ = false;
    std::array<float, 240> frame_ms_ = {};
    std::size_t next_ = 0;
};

// The eye and the four far corners of a camera's frustum, along its view up to a distance.
std::array<vgl_dvec3, 5>
frustum_of(const camera& c, double field_of_view, double aspect, double distance);

} // namespace sample

#endif // VGL_SAMPLES_GL_OVERLAY_HPP
