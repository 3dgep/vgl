// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// OpenGL sample (design document, section 11): the globe drawn by the OpenGL backend in a GLFW
// window, on OpenGL 3.3 or, with --es, on OpenGL ES 3.0. The host owns the window, the context,
// the camera and the framebuffer; vgl draws into it.
//
//   vgl-gl [--data <directory> | --pack <uri>...] [--cache <directory> [--cache-budget <MiB>]]
//          [--es] [--egl] [--depth logarithmic|reversed|conventional] [--check]
//          [--save <directory>]
//
// It draws the sample set (samples/data/README.md) from the directory given with --data, or else
// from the one in the environment variable VGL_SAMPLE_DATA, or else from the source tree's
// samples/data. Instead, --pack, repeated, draws the packs named, each winning over the ones
// before it where they overlap (samples/common/sample_data.hpp).
//
// Controls, as in Google Earth (samples/common/navigation.hpp, shared by the samples): drag to grab
// the globe, scroll or double-click to zoom to the cursor, shift-drag or middle-drag to orbit and
// tilt, ctrl-drag to look around, right-drag to zoom; arrows or WASD move, with shift they turn and
// tilt, page up and down zoom; N turns north up, U looks straight down, R does both. 1 to 7 jump to
// places, and L switches between sunlight and a headlight. A panel shows the context's statistics
// and the debug views: F1 shows or hides it, F2 the wireframe, F3 colours by tile level, F4 the
// imagery source, F5 freezes the selection, so that tiles are selected for the frozen camera while
// you fly around it, F6 colours by height instead of imagery, F7 draws contour lines, F8 a sea
// surface over the ocean floor, and F9 the atmosphere.
//
// Built with Emscripten, it runs in a browser on WebGL 2 (samples/gl/web/README.md), with the
// options in the page's address: vgl-gl.html?pack=<uri>&pack=<uri>&depth=reversed.
//
// With --check, the sample runs hidden: it flies to a few views with each depth convention
// the device supports, and checks what it draws, that the conventions agree, that OpenGL
// reports no errors and that the backend restores the host's state. It exits with 1 on a
// failure, so that it doubles as a test of the backend. --save writes the images it checks as
// PPM files.

#define GLFW_INCLUDE_NONE
#include "clouds.hpp"
#include "glfw_controls.hpp"
#include "navigation.hpp"
#include "overlay.hpp"
#include "pack_reader.hpp"
#include "sample_camera.hpp"
#include "sample_check.hpp"
#include "sample_data.hpp"
#include "stars.hpp"
#include <imgui_impl_opengl3.h>

#include <vgl/defaults.hpp>
#include <vgl/gl.hpp>
#include <vgl/vgl.hpp>

#include <GLFW/glfw3.h>

#if defined(__EMSCRIPTEN__)
#    include <emscripten/emscripten.h>

#    include <GLFW/emscripten_glfw3.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <imgui_impl_glfw.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef VGL_SAMPLE_DATA_DIR
#    define VGL_SAMPLE_DATA_DIR "."
#endif

namespace
{

using sample::clear_color;
using sample::contour_interval_for;
using sample::depth_name;
using sample::expect;
using sample::field_of_view;
using sample::image;
using sample::place;
using sample::places;

// ---------------------------------------------------------------------------------------------
// The OpenGL the host itself uses: clearing, its framebuffer, and the checks.
// ---------------------------------------------------------------------------------------------

using GLenum = std::uint32_t;
using GLint = std::int32_t;
using GLuint = std::uint32_t;
using GLsizei = std::int32_t;
using GLboolean = std::uint8_t;

constexpr GLenum GL_DEPTH_BUFFER_BIT = 0x0100;
constexpr GLenum GL_COLOR_BUFFER_BIT = 0x4000;
constexpr GLenum GL_FRONT = 0x0404;
constexpr GLenum GL_CW = 0x0900;
constexpr GLenum GL_ALWAYS = 0x0207;
constexpr GLenum GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_CULL_FACE_MODE = 0x0B45;
constexpr GLenum GL_FRONT_FACE = 0x0B46;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLenum GL_DEPTH_WRITEMASK = 0x0B72;
constexpr GLenum GL_DEPTH_FUNC = 0x0B74;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_SRC_ALPHA = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_BLEND_DST_RGB = 0x80C8;
constexpr GLenum GL_BLEND_SRC_RGB = 0x80C9;
constexpr GLenum GL_BLEND_DST_ALPHA = 0x80CA;
constexpr GLenum GL_BLEND_SRC_ALPHA = 0x80CB;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_NEAREST = 0x2600;
constexpr GLenum GL_RENDERER = 0x1F01;
constexpr GLenum GL_VERSION = 0x1F02;
constexpr GLenum GL_RGBA8 = 0x8058;
constexpr GLenum GL_DEPTH_COMPONENT24 = 0x81A6;
constexpr GLenum GL_VERTEX_ARRAY_BINDING = 0x85B5;
constexpr GLenum GL_ARRAY_BUFFER_BINDING = 0x8894;
constexpr GLenum GL_CURRENT_PROGRAM = 0x8B8D;
constexpr GLenum GL_LOWER_LEFT = 0x8CA1;
constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum GL_DEPTH_COMPONENT32F = 0x8CAC;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_RENDERBUFFER = 0x8D41;
constexpr GLenum GL_CLIP_DEPTH_MODE = 0x935D;
constexpr GLenum GL_NEGATIVE_ONE_TO_ONE = 0x935E;
constexpr GLenum GL_ZERO_TO_ONE = 0x935F;

struct host_gl
{
    const unsigned char* (*GetString)(GLenum) = nullptr;
    GLenum (*GetError)() = nullptr;
    void (*GetIntegerv)(GLenum, GLint*) = nullptr;
    GLboolean (*IsEnabled)(GLenum) = nullptr;
    void (*Enable)(GLenum) = nullptr;
    void (*Disable)(GLenum) = nullptr;
    void (*DepthFunc)(GLenum) = nullptr;
    void (*DepthMask)(GLboolean) = nullptr;
    void (*CullFace)(GLenum) = nullptr;
    void (*FrontFace)(GLenum) = nullptr;
    void (*BlendFunc)(GLenum, GLenum) = nullptr;
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void (*ClearColor)(float, float, float, float) = nullptr;
    void (*ClearDepthf)(float) = nullptr; // OpenGL ES and OpenGL 4.1
    void (*ClearDepth)(double) = nullptr; // OpenGL
    void (*Clear)(GLenum) = nullptr;
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
    void (*GenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void (*DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void (*BindFramebuffer)(GLenum, GLuint) = nullptr;
    GLenum (*CheckFramebufferStatus)(GLenum) = nullptr;
    void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
    void (*GenRenderbuffers)(GLsizei, GLuint*) = nullptr;
    void (*DeleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;
    void (*BindRenderbuffer)(GLenum, GLuint) = nullptr;
    void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
    void (*BlitFramebuffer)(
        GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLenum, GLenum) = nullptr;
    void (*GenVertexArrays)(GLsizei, GLuint*) = nullptr;
    void (*DeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
    void (*BindVertexArray)(GLuint) = nullptr;
    void (*ClipControl)(GLenum, GLenum) = nullptr; // Optional.
};

template <typename F>
void load(F& f, const char* name)
{
    f = reinterpret_cast<F>(glfwGetProcAddress(name));
}

host_gl load_host_gl()
{
    host_gl gl;
    load(gl.GetString, "glGetString");
    load(gl.GetError, "glGetError");
    load(gl.GetIntegerv, "glGetIntegerv");
    load(gl.IsEnabled, "glIsEnabled");
    load(gl.Enable, "glEnable");
    load(gl.Disable, "glDisable");
    load(gl.DepthFunc, "glDepthFunc");
    load(gl.DepthMask, "glDepthMask");
    load(gl.CullFace, "glCullFace");
    load(gl.FrontFace, "glFrontFace");
    load(gl.BlendFunc, "glBlendFunc");
    load(gl.Viewport, "glViewport");
    load(gl.ClearColor, "glClearColor");
    load(gl.ClearDepthf, "glClearDepthf");
    load(gl.ClearDepth, "glClearDepth");
    load(gl.Clear, "glClear");
    load(gl.ReadPixels, "glReadPixels");
    load(gl.GenFramebuffers, "glGenFramebuffers");
    load(gl.DeleteFramebuffers, "glDeleteFramebuffers");
    load(gl.BindFramebuffer, "glBindFramebuffer");
    load(gl.CheckFramebufferStatus, "glCheckFramebufferStatus");
    load(gl.FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    load(gl.GenRenderbuffers, "glGenRenderbuffers");
    load(gl.DeleteRenderbuffers, "glDeleteRenderbuffers");
    load(gl.BindRenderbuffer, "glBindRenderbuffer");
    load(gl.RenderbufferStorage, "glRenderbufferStorage");
    load(gl.BlitFramebuffer, "glBlitFramebuffer");
    load(gl.GenVertexArrays, "glGenVertexArrays");
    load(gl.DeleteVertexArrays, "glDeleteVertexArrays");
    load(gl.BindVertexArray, "glBindVertexArray");
    load(gl.ClipControl, "glClipControl");
    if (gl.ClipControl == nullptr)
    {
        load(gl.ClipControl, "glClipControlEXT");
    }
    return gl;
}

// The backend's loader: GLFW's, with the user data vgl's callbacks take.
vgl_gl_proc get_proc_address(void* /*user_data*/, const char* name)
{
    return reinterpret_cast<vgl_gl_proc>(glfwGetProcAddress(name));
}

// ---------------------------------------------------------------------------------------------
// The host's framebuffer: color and depth, with a 32-bit floating-point depth buffer for the
// reversed-Z convention. It is drawn into, then copied to the window.
// ---------------------------------------------------------------------------------------------

struct target
{
    GLuint framebuffer = 0;
    GLuint color = 0;
    GLuint depth = 0;
    int width = 0;
    int height = 0;
};

bool target_create(const host_gl& gl, target& t, int width, int height, bool float_depth)
{
    t.width = width;
    t.height = height;
    gl.GenRenderbuffers(1, &t.color);
    gl.BindRenderbuffer(GL_RENDERBUFFER, t.color);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    gl.GenRenderbuffers(1, &t.depth);
    gl.BindRenderbuffer(GL_RENDERBUFFER, t.depth);
    gl.RenderbufferStorage(
        GL_RENDERBUFFER, float_depth ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT24, width, height);
    gl.GenFramebuffers(1, &t.framebuffer);
    gl.BindFramebuffer(GL_FRAMEBUFFER, t.framebuffer);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t.color);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.depth);
    return gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void target_destroy(const host_gl& gl, target& t)
{
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.DeleteFramebuffers(1, &t.framebuffer);
    gl.DeleteRenderbuffers(1, &t.color);
    gl.DeleteRenderbuffers(1, &t.depth);
    t = target{};
}

// ---------------------------------------------------------------------------------------------
// Drawing a frame
// ---------------------------------------------------------------------------------------------

struct scene
{
    vgl_depth_convention depth = VGL_DEPTH_CONVENTION_LOGARITHMIC;
    sample::camera camera;
    sample::camera frozen; // Where tiles are selected for while the selection is frozen.
    sample::debug_settings debug;
};

vgl_dmat4 projection_for(const scene& s, const sample::camera& c, double aspect)
{
    const double fovy = vgl::degrees_to_radians(field_of_view);
    switch (s.depth)
    {
    case VGL_DEPTH_CONVENTION_REVERSED_Z:
        return sample::perspective_reversed(fovy, aspect, 1.0);
    case VGL_DEPTH_CONVENTION_CONVENTIONAL:
        // A near plane that grows with the altitude, which is all a conventional depth buffer
        // can do at this scale.
        return sample::perspective(
            fovy, aspect, std::max(1.0, c.altitude * 0.05), c.altitude + 2.0e7);
    default:
        // The logarithmic convention writes its own depth; the planes do not matter.
        return sample::perspective(fovy, aspect, 1.0, 1.0e9);
    }
}

// Sets the view that the next update selects tiles for: the camera's, or the frozen one's.
void view_set(vgl::context& ctx, const scene& s, int width, int height)
{
    const sample::camera& c = s.debug.freeze ? s.frozen : s.camera;
    auto view = vgl::init<vgl_view>();
    view.view_matrix = sample::camera_view_matrix(c);
    view.projection_matrix = projection_for(s, c, static_cast<double>(width) / height);
    view.viewport_width = static_cast<std::uint32_t>(width);
    view.viewport_height = static_cast<std::uint32_t>(height);
    view.max_screen_space_error = s.debug.max_error;
    static_cast<void>(ctx.view_set(&view));
}

// Clears the host's framebuffer for the depth convention.
void clear(const host_gl& gl, const scene& s, const target& t)
{
    gl.BindFramebuffer(GL_FRAMEBUFFER, t.framebuffer);
    gl.Viewport(0, 0, t.width, t.height);
    gl.ClearColor(clear_color[0], clear_color[1], clear_color[2], 1.0f);
    const float clear_depth = s.depth == VGL_DEPTH_CONVENTION_REVERSED_Z ? 0.0f : 1.0f;
    if (gl.ClearDepthf != nullptr)
    {
        gl.ClearDepthf(clear_depth);
    }
    else
    {
        gl.ClearDepth(clear_depth);
    }
    gl.DepthMask(1);
    gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// Has the backend draw the globe into the bound framebuffer, from the live camera, which
// differs from the view the tiles were selected for while the selection is frozen.
vgl::result draw(vgl::gl_backend& backend, const scene& s, int width, int height)
{
    const vgl_dmat4 view = sample::camera_view_matrix(s.camera);
    const vgl_dmat4 projection = projection_for(s, s.camera, static_cast<double>(width) / height);
    auto info = vgl::init<vgl_gl_draw_info>();
    info.view_matrix = &view;
    info.projection_matrix = &projection;
    if (s.debug.sunlight)
    {
        info.sun_direction = sample::sun_direction(s.debug);
    }
    info.debug_flags = sample::debug_flags(s.debug);
    info.contour_interval = s.debug.contour_interval;
    info.sea_surface = s.debug.sea ? VGL_TRUE : VGL_FALSE;
    info.atmosphere = s.debug.atmosphere ? VGL_TRUE : VGL_FALSE;
    info.stars = s.debug.stars ? VGL_TRUE : VGL_FALSE;
    info.clouds = s.debug.clouds ? VGL_TRUE : VGL_FALSE;
    info.sidereal_angle =
        sample::sidereal_angle(s.debug.time_of_day, s.debug.day_of_year, s.debug.year);
    return backend.draw(&info);
}

// The host's clip control, for its own geometry: the reversed-Z convention needs depth from 0
// to 1. The backend keeps whatever the host sets, and restores it.
bool depth_mode_set(const host_gl& gl, vgl_depth_convention depth)
{
    if (gl.ClipControl == nullptr)
    {
        return depth != VGL_DEPTH_CONVENTION_REVERSED_Z;
    }
    gl.ClipControl(GL_LOWER_LEFT,
                   depth == VGL_DEPTH_CONVENTION_REVERSED_Z ? GL_ZERO_TO_ONE
                                                            : GL_NEGATIVE_ONE_TO_ONE);
    return true;
}

// ---------------------------------------------------------------------------------------------
// The check
// ---------------------------------------------------------------------------------------------

// Host state set before each draw in the check, which the backend must leave as it was.
void sentinel_state_set(const host_gl& gl, GLuint vertex_array)
{
    gl.BindVertexArray(vertex_array);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.Disable(GL_DEPTH_TEST);
    gl.DepthFunc(GL_ALWAYS);
    gl.DepthMask(0);
    gl.Disable(GL_CULL_FACE);
    gl.CullFace(GL_FRONT);
    gl.FrontFace(GL_CW);
}

void sentinel_state_check(const host_gl& gl, GLuint vertex_array, vgl_depth_convention depth)
{
    GLint v = 0;
    gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &v);
    expect(static_cast<GLuint>(v) == vertex_array, "the vertex array binding is restored");
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &v);
    expect(v == 0, "the program is restored");
    gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &v);
    expect(v == 0, "the array buffer binding is restored");
    expect(gl.IsEnabled(GL_BLEND) != 0, "blending is restored");
    GLint src_rgb = 0;
    GLint dst_rgb = 0;
    GLint src_alpha = 0;
    GLint dst_alpha = 0;
    gl.GetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    gl.GetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    gl.GetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha);
    gl.GetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
    expect(static_cast<GLenum>(src_rgb) == GL_SRC_ALPHA &&
               static_cast<GLenum>(dst_rgb) == GL_ONE_MINUS_SRC_ALPHA &&
               static_cast<GLenum>(src_alpha) == GL_SRC_ALPHA &&
               static_cast<GLenum>(dst_alpha) == GL_ONE_MINUS_SRC_ALPHA,
           "the blend functions are restored");
    expect(gl.IsEnabled(GL_DEPTH_TEST) == 0, "the depth test is restored");
    expect(gl.IsEnabled(GL_CULL_FACE) == 0, "culling is restored");
    gl.GetIntegerv(GL_DEPTH_FUNC, &v);
    expect(static_cast<GLenum>(v) == GL_ALWAYS, "the depth function is restored");
    gl.GetIntegerv(GL_DEPTH_WRITEMASK, &v);
    expect(v == 0, "the depth write mask is restored");
    gl.GetIntegerv(GL_CULL_FACE_MODE, &v);
    expect(static_cast<GLenum>(v) == GL_FRONT, "the cull face mode is restored");
    gl.GetIntegerv(GL_FRONT_FACE, &v);
    expect(static_cast<GLenum>(v) == GL_CW, "the front face is restored");
    if (gl.ClipControl != nullptr)
    {
        gl.GetIntegerv(GL_CLIP_DEPTH_MODE, &v);
        const GLenum expected =
            depth == VGL_DEPTH_CONVENTION_REVERSED_Z ? GL_ZERO_TO_ONE : GL_NEGATIVE_ONE_TO_ONE;
        expect(static_cast<GLenum>(v) == expected, "the clip control depth mode is restored");
    }
}

// The framebuffer's pixels, rows from the top: OpenGL reads them from the bottom.
image read_image(const host_gl& gl, const target& t)
{
    image i;
    i.width = t.width;
    i.height = t.height;
    const std::size_t row = static_cast<std::size_t>(t.width) * 4;
    std::vector<std::uint8_t> bottom_up(row * static_cast<std::size_t>(t.height));
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, t.framebuffer);
    gl.ReadPixels(0, 0, t.width, t.height, GL_RGBA, GL_UNSIGNED_BYTE, bottom_up.data());
    i.pixels.resize(bottom_up.size());
    for (int y = 0; y < t.height; ++y)
    {
        std::copy_n(bottom_up.data() + static_cast<std::size_t>(t.height - 1 - y) * row,
                    row,
                    i.pixels.data() + static_cast<std::size_t>(y) * row);
    }
    return i;
}

// Where an ECEF point appears in a window, for a camera; false behind it.
bool project(const sample::camera& c, const vgl_dvec3& p, double w, double h, double& x, double& y)
{
    const vgl_dmat4 v = sample::camera_view_matrix(c);
    const vgl_dmat4 m =
        sample::perspective(vgl::degrees_to_radians(field_of_view), w / h, 1.0, 1.0e9);
    const double cx = v.m[0] * p.x + v.m[4] * p.y + v.m[8] * p.z + v.m[12];
    const double cy = v.m[1] * p.x + v.m[5] * p.y + v.m[9] * p.z + v.m[13];
    const double cz = v.m[2] * p.x + v.m[6] * p.y + v.m[10] * p.z + v.m[14];
    if (cz >= 0.0)
    {
        return false;
    }
    x = (m.m[0] * cx / -cz * 0.5 + 0.5) * w;
    y = (0.5 - m.m[5] * cy / -cz * 0.5) * h;
    return true;
}

// The navigation's promises (navigation.hpp), from three cameras: a grabbed point stays under
// the cursor, zooming keeps the point under the cursor in place, orbiting keeps the point at
// the centre in place and never tilts past the limit, and the camera stays above the ground.
void navigation_check()
{
    constexpr double w = 1280.0;
    constexpr double h = 720.0;
    using drag = sample::globe_navigation::drag;
    const std::array<sample::camera, 4> starts{{
        {30.0, -30.0, 2.0e7, 0.0, 90.0},
        {44.5, 9.0, 3.0e5, 0.0, 35.0},
        {27.0, 86.9, 2.0e4, 45.0, 60.0},
        {61.5, 7.5, 2.0e4, 0.0, 90.0},
    }};
    const auto near = [](double x, double y, double ex, double ey) {
        return std::abs(x - ex) < 1.0 && std::abs(y - ey) < 1.0;
    };
    for (const sample::camera& start : starts)
    {
        const std::string where =
            " from " + std::to_string(static_cast<int>(start.altitude / 1000.0)) + " km at " +
            std::to_string(static_cast<int>(start.pitch)) + " degrees";
        sample::globe_navigation nav;
        nav.viewport(w, h, field_of_view);
        nav.ground(0.0);

        // Grab and drag in steps.
        sample::camera c = start;
        vgl_dvec3 grabbed{};
        expect(nav.pick(c, 600.0, 420.0, grabbed), "the cursor is over the globe" + where);
        nav.press(drag::pan, 600.0, 420.0, c);
        for (int k = 1; k <= 10; ++k)
        {
            nav.move(600.0 + 12.0 * k, 420.0 - 8.0 * k, c);
        }
        nav.release();
        double x = 0.0;
        double y = 0.0;
        expect(project(c, grabbed, w, h, x, y) && near(x, y, 720.0, 340.0),
               "a grabbed point stays under the cursor" + where);

        // The same drag while the terrain under the camera changes, as it does with every move:
        // the height taken when the drag began holds until it ends, so the camera turns the same.
        sample::camera bumpy = start;
        nav.press(drag::pan, 600.0, 420.0, bumpy);
        for (int k = 1; k <= 10; ++k)
        {
            nav.ground(150.0 * k);
            nav.move(600.0 + 12.0 * k, 420.0 - 8.0 * k, bumpy);
        }
        nav.release();
        nav.ground(0.0);
        expect(std::abs(bumpy.heading - c.heading) < 1e-6 &&
                   std::abs(bumpy.pitch - c.pitch) < 1e-6 &&
                   std::abs(bumpy.latitude - c.latitude) < 1e-9 &&
                   std::abs(bumpy.longitude - c.longitude) < 1e-9,
               "a drag holds the ground height it began with" + where);

        // A throw, from a steady drag with uneven frames and mouse events that do not line up
        // with them: the globe carries on turning at the speed of the drag's last tenth of a
        // second, and slows down smoothly.
        {
            const auto turned = [](const sample::camera& from, const sample::camera& to) {
                const vgl_dvec3 a = sample::normalize(sample::camera_eye(from));
                const vgl_dvec3 b = sample::normalize(sample::camera_eye(to));
                const vgl_dvec3 axis = sample::cross(a, b);
                return std::atan2(std::sqrt(sample::dot(axis, axis)), sample::dot(a, b));
            };
            constexpr std::array<double, 5> frames{0.002, 0.009, 0.004, 0.0015, 0.012};
            sample::camera t = start;
            nav.press(drag::pan, 600.0, 420.0, t);
            double now = 0.0;
            double mouse = 0.0; // Time of the next mouse event, every 8 ms.
            int events = 0;
            std::size_t frame = 0;
            sample::camera mid = t;
            double mid_time = 0.0;
            while (now < 0.4)
            {
                now += frames[frame++ % frames.size()];
                for (; mouse <= now; mouse += 0.008)
                {
                    ++events;
                    nav.move(600.0 + 3.0 * events, 420.0 - 3.0 * events, t);
                }
                nav.update(frames[(frame - 1) % frames.size()], {}, t);
                if (mid_time == 0.0 && now >= 0.3)
                {
                    mid = t;
                    mid_time = now;
                }
            }
            const double drag_rate = turned(mid, t) / (now - mid_time);
            nav.release();
            bool smooth = true;
            double first = 0.0;
            double last = 0.0;
            double lasted = 0.0;
            for (int k = 0; k < 400; ++k)
            {
                const double dt = frames[frame++ % frames.size()];
                const sample::camera before = t;
                nav.update(dt, {}, t);
                const double rate = turned(before, t) / dt;
                if (rate == 0.0)
                {
                    break;
                }
                lasted += dt;
                if (first == 0.0)
                {
                    first = rate;
                }
                else
                {
                    smooth = smooth && rate <= last * (1.0 + 1e-6) && rate >= last * 0.9;
                }
                last = rate;
            }
            expect(std::abs(first - drag_rate) < 0.1 * drag_rate,
                   "a throw carries on at the drag's speed" + where);
            expect(smooth && lasted > 0.5, "a throw slows down smoothly" + where);
            nav.stop();

            // Holding still before letting go does not throw.
            nav.press(drag::pan, 600.0, 420.0, t);
            for (int k = 1; k <= 20; ++k)
            {
                nav.move(600.0 + 3.0 * k, 420.0 - 3.0 * k, t);
                nav.update(0.008, {}, t);
            }
            for (int k = 0; k < 20; ++k)
            {
                nav.update(0.008, {}, t);
            }
            nav.release();
            const sample::camera held = t;
            nav.update(0.008, {}, t);
            nav.update(0.008, {}, t);
            expect(turned(held, t) == 0.0, "letting go after holding still does not throw" + where);

            // The same throw over 0.6 seconds in even frames and in very uneven ones ends in the
            // same place: how far it goes depends on the time alone.
            const auto thrown = [&](const std::vector<double>& steps) {
                sample::camera th = start;
                nav.stop();
                nav.press(drag::pan, 600.0, 420.0, th);
                for (int k = 1; k <= 20; ++k)
                {
                    nav.move(600.0 + 3.0 * k, 420.0 - 3.0 * k, th);
                    nav.update(0.008, {}, th);
                }
                nav.release();
                for (const double dt : steps)
                {
                    nav.update(dt, {}, th);
                }
                nav.stop();
                return th;
            };
            const sample::camera even = thrown(std::vector<double>(24, 0.025));
            std::vector<double> uneven;
            for (int k = 0; k < 6; ++k)
            {
                uneven.insert(uneven.end(), {0.002, 0.048, 0.01, 0.04});
            }
            const sample::camera bumpy_throw = thrown(uneven);
            expect(turned(even, bumpy_throw) < 1e-4 * turned(start, even),
                   "a throw goes as far in uneven frames as in even ones" + where);
        }

        // Zoom towards the cursor.
        c = start;
        vgl_dvec3 under{};
        // A point off the centre that is on the globe from every start.
        const double zx = w / 2.0 + 90.0;
        const double zy = h / 2.0 + 60.0;
        expect(nav.pick(c, zx, zy, under), "the cursor is over the globe" + where);
        nav.scroll(2.0, zx, zy, c);
        for (int k = 0; k < 120; ++k)
        {
            nav.update(1.0 / 60.0, {}, c);
        }
        expect(c.altitude < start.altitude, "scrolling up zooms in" + where);
        expect(project(c, under, w, h, x, y) && near(x, y, zx, zy),
               "zooming keeps the point under the cursor" + where);

        // The same zoom while the terrain under the camera changes: a smooth zoom holds the
        // ground height it began with, like a drag, so that it zooms the same.
        sample::camera rising = start;
        nav.scroll(2.0, zx, zy, rising);
        for (int k = 0; k < 120; ++k)
        {
            nav.ground(10.0 * k);
            nav.update(1.0 / 60.0, {}, rising);
        }
        nav.ground(0.0);
        expect(std::abs(rising.heading - c.heading) < 1e-6 &&
                   std::abs(rising.pitch - c.pitch) < 1e-6 &&
                   std::abs(rising.latitude - c.latitude) < 1e-9 &&
                   std::abs(rising.longitude - c.longitude) < 1e-9,
               "a smooth zoom holds the ground height it began with" + where);

        // The same zoom over a third of a second, partway, in even frames and in very uneven
        // ones: how far it has gone depends on the time alone.
        const auto zoomed = [&](const std::vector<double>& steps) {
            sample::camera z = start;
            nav.stop();
            nav.scroll(2.0, zx, zy, z);
            for (const double dt : steps)
            {
                nav.update(dt, {}, z);
            }
            nav.stop();
            return z;
        };
        const sample::camera zoom_even = zoomed(std::vector<double>(10, 0.03));
        std::vector<double> zoom_uneven;
        for (int k = 0; k < 3; ++k)
        {
            zoom_uneven.insert(zoom_uneven.end(), {0.002, 0.07, 0.028});
        }
        const sample::camera zoom_bumpy = zoomed(zoom_uneven);
        // The same height, and the same view to within a hundredth of a pixel at the centre and
        // off it.
        bool same_view = std::abs(zoom_bumpy.altitude - zoom_even.altitude) <
                         1e-6 * std::abs(start.altitude - zoom_even.altitude);
        for (const auto& [px, py] :
             {std::pair{w / 2.0, h / 2.0}, std::pair{w / 2.0 + 120.0, h / 2.0 + 90.0}})
        {
            vgl_dvec3 seen{};
            same_view = same_view && nav.pick(zoom_even, px, py, seen) &&
                        project(zoom_bumpy, seen, w, h, x, y) && std::abs(x - px) < 0.01 &&
                        std::abs(y - py) < 0.01;
        }
        expect(same_view, "a zoom goes as far in uneven frames as in even ones" + where);

        // Keys move over the ground as fast forward as sideways, at the same height, as far in
        // uneven frames as in even ones, and hold the ground height they began with.
        {
            const auto travelled = [](const sample::camera& from, const sample::camera& to) {
                const vgl_dvec3 a = sample::normalize(sample::camera_eye(from));
                const vgl_dvec3 b = sample::normalize(sample::camera_eye(to));
                const vgl_dvec3 axis = sample::cross(a, b);
                return std::atan2(std::sqrt(sample::dot(axis, axis)), sample::dot(a, b));
            };
            const auto keyed = [&](const sample::navigation_keys& keys,
                                   const std::vector<double>& steps,
                                   bool ground_rises) {
                sample::camera k = start;
                nav.stop();
                for (std::size_t i = 0; i < steps.size(); ++i)
                {
                    if (ground_rises)
                    {
                        nav.ground(10.0 * static_cast<double>(i));
                    }
                    nav.update(steps[i], keys, k);
                }
                nav.ground(0.0);
                sample::camera idle = k;
                nav.update(0.01, {}, idle); // Keys up: the latest ground height applies again.
                return k;
            };
            sample::navigation_keys forward;
            forward.up = true;
            sample::navigation_keys sideways;
            sideways.right = true;
            const std::vector<double> even(30, 0.02);
            std::vector<double> uneven;
            for (int k = 0; k < 6; ++k)
            {
                uneven.insert(uneven.end(), {0.002, 0.05, 0.013, 0.035});
            }
            const sample::camera ahead = keyed(forward, even, false);
            const sample::camera aside = keyed(sideways, even, false);
            const double d_ahead = travelled(start, ahead);
            const double d_aside = travelled(start, aside);
            expect(d_ahead > 0.0 && std::abs(d_ahead - d_aside) < 1e-4 * d_ahead,
                   "keys move as fast forward as sideways" + where);
            expect(std::abs(ahead.altitude - start.altitude) < 1e-6 &&
                       std::abs(aside.altitude - start.altitude) < 1e-6,
                   "keys keep the height" + where);
            const sample::camera ahead_uneven = keyed(forward, uneven, false);
            bool same = true;
            for (const auto& [px, py] :
                 {std::pair{w / 2.0, h / 2.0}, std::pair{w / 2.0 + 120.0, h / 2.0 + 90.0}})
            {
                vgl_dvec3 seen{};
                same = same && nav.pick(ahead, px, py, seen) &&
                       project(ahead_uneven, seen, w, h, x, y) && std::abs(x - px) < 0.01 &&
                       std::abs(y - py) < 0.01;
            }
            expect(same, "keys move as far in uneven frames as in even ones" + where);
            const sample::camera ahead_rising = keyed(forward, even, true);
            expect(travelled(ahead, ahead_rising) < 1e-6 * d_ahead,
                   "keys hold the ground height they began with" + where);

            // Holding a key speeds up smoothly to four view heights per second over about two
            // seconds; letting go stops within a fraction of one.
            constexpr double top = 4.0;
            const double view =
                start.altitude * 2.0 * std::tan(vgl::degrees_to_radians(field_of_view) / 2.0);
            sample::camera k = start;
            nav.stop();
            constexpr double dt = 0.01;
            double speed = 0.0;
            double at_tenth = 0.0;
            double at_one = 0.0;
            bool rising_smoothly = true;
            for (int i = 1; i <= 200; ++i)
            {
                const sample::camera before = k;
                nav.update(dt, forward, k);
                const double now_speed = travelled(before, k) * 6378137.0 / dt / view;
                rising_smoothly =
                    rising_smoothly && now_speed >= speed && now_speed - speed < 0.05 * top;
                speed = now_speed;
                at_tenth = i == 10 ? speed : at_tenth;
                at_one = i == 100 ? speed : at_one;
            }
            expect(rising_smoothly && at_tenth < 0.1 * top && at_one > 0.75 * top &&
                       at_one < 0.9 * top && speed > 0.97 * top && speed < top * 1.0001,
                   "holding a key speeds up smoothly" + where);
            const double held_speed = speed;
            double after_stop = 0.0;
            bool stopped = false;
            for (int i = 1; i <= 50; ++i)
            {
                const sample::camera before = k;
                nav.update(dt, {}, k);
                speed = travelled(before, k) * 6378137.0 / dt / view;
                after_stop = i == 15 ? speed : after_stop;
                stopped = speed == 0.0;
            }
            expect(after_stop < 0.05 * held_speed && stopped,
                   "letting go of a key stops quickly" + where);
            nav.stop();
        }

        // Orbit around the centre, tilting as far as it goes.
        c = start;
        vgl_dvec3 centre{};
        expect(nav.pick(c, w / 2.0, h / 2.0, centre), "the centre is on the globe" + where);
        nav.press(drag::orbit, w / 2.0, h / 2.0, c);
        for (int k = 1; k <= 40; ++k)
        {
            nav.move(w / 2.0 + 5.0 * k, h / 2.0 - 10.0 * k, c);
        }
        nav.release();
        expect(project(c, centre, w, h, x, y) && near(x, y, w / 2.0, h / 2.0),
               "orbiting keeps the centre in place" + where);
        const vgl_dvec3 eye = sample::camera_eye(c);
        const vgl_dvec3 up = sample::normalize(centre);
        const double tilt = vgl::radians_to_degrees(
            std::acos(sample::dot(up, sample::normalize(sample::sub(eye, centre)))));
        expect(tilt < 86.0, "orbiting stops at the tilt limit" + where);
        expect(c.altitude >= 20.0, "the camera stays above the ground" + where);

        // Tilt only, both ways, through straight down: the view stops there instead of turning
        // over, so the heading never jumps.
        for (const double step : {2.0, -2.0})
        {
            c = start;
            nav.press(drag::orbit, w / 2.0, h / 2.0, c);
            bool steady = true;
            for (int k = 1; k <= 200; ++k)
            {
                nav.move(w / 2.0, h / 2.0 + step * k, c);
                const double turned = std::abs(std::remainder(c.heading - start.heading, 360.0));
                steady = steady && turned < 1.0 && c.pitch <= 90.0;
            }
            nav.release();
            expect(steady, "tilting never turns the view over" + where);
        }

        // Orbit around a point off the centre: it stays under the cursor while the camera turns
        // and tilts, also through straight down, and the view never turns over.
        {
            const double px = w / 2.0 + 90.0;
            const double py = h / 2.0 + 60.0;
            for (const auto& [mx, my] :
                 {std::pair{5.0, -3.0}, std::pair{0.0, 2.0}, std::pair{0.0, -2.0}})
            {
                c = start;
                vgl_dvec3 pivot{};
                expect(nav.pick(c, px, py, pivot), "the cursor is over the globe" + where);
                nav.press(drag::orbit, px, py, c);
                bool held = true;
                bool upright = true;
                double previous = c.heading;
                for (int k = 1; k <= 120; ++k)
                {
                    nav.move(px + mx * k, py + my * k, c);
                    held = held && project(c, pivot, w, h, x, y) && near(x, y, px, py);
                    upright = upright && c.pitch <= 90.0 &&
                              std::abs(std::remainder(c.heading - previous, 360.0)) < 90.0;
                    previous = c.heading;
                }
                nav.release();
                expect(held, "orbiting keeps the clicked point under the cursor" + where);
                expect(upright,
                       "orbiting around the clicked point never turns the view over" + where);
            }
        }

        // The reticle: at the clicked point while the mouse orbits around it, then fading over
        // half a second; at the cursor while a scroll zooms towards it, until the zoom ends.
        {
            nav.stop();
            c = start;
            const double px = w / 2.0 + 90.0;
            const double py = h / 2.0 + 60.0;
            double fx = 0.0;
            double fy = 0.0;
            double strength = 0.0;
            nav.press(drag::orbit, px, py, c);
            nav.move(px + 10.0, py, c);
            nav.update(0.01, {}, c);
            const bool orbiting =
                nav.focus(c, fx, fy, strength) && near(fx, fy, px, py) && strength == 1.0;
            nav.release();
            nav.update(0.25, {}, c);
            const bool fading = nav.focus(c, fx, fy, strength) && strength > 0.4 && strength < 0.6;
            nav.update(0.3, {}, c);
            const bool gone = !nav.focus(c, fx, fy, strength);
            expect(orbiting && fading && gone,
                   "the reticle shows the orbit's point, then fades" + where);

            c = start;
            nav.scroll(1.0, zx, zy, c);
            nav.update(0.05, {}, c);
            const bool zooming =
                nav.focus(c, fx, fy, strength) && near(fx, fy, zx, zy) && strength == 1.0;
            for (int k = 0; k < 200; ++k)
            {
                nav.update(0.01, {}, c);
            }
            const bool done = !nav.focus(c, fx, fy, strength);
            expect(zooming && done, "the reticle shows a zoom's point until it ends" + where);
            nav.stop();
        }

        // Pressed over space, a drag turns the globe like a disk about the line to the camera,
        // following the cursor around it, which keeps the camera in place; once the cursor
        // reaches the globe it grabs the point there, and leaving it again spins once more, as
        // does a drag pressed over the globe that leaves it.
        c = start;
        vgl_dvec3 sky{};
        if (!nav.pick(c, 5.0, 5.0, sky))
        {
            // The disk is the plane through the globe's centre that faces the screen. The point
            // of the globe to hold is where the line from the cursor's place on the disk to the
            // centre meets the ellipsoid, which may lie behind the horizon.
            const vgl_dvec3 origin{0.0, 0.0, 0.0};
            double cx = 0.0;
            double cy = 0.0;
            expect(project(c, origin, w, h, cx, cy), "the globe's centre is in front" + where);
            const vgl_dmat4 v = sample::camera_view_matrix(c);
            const double tangent = std::tan(vgl::degrees_to_radians(field_of_view) / 2.0);
            const double sx = (2.0 * 5.0 / w - 1.0) * tangent * (w / h);
            const double sy = (1.0 - 2.0 * 5.0 / h) * tangent;
            const vgl_dvec3 d = sample::normalize({v.m[0] * sx + v.m[1] * sy - v.m[2],
                                                   v.m[4] * sx + v.m[5] * sy - v.m[6],
                                                   v.m[8] * sx + v.m[9] * sy - v.m[10]});
            const vgl_dvec3 f{-v.m[2], -v.m[6], -v.m[10]};
            const vgl_dvec3 viewer = sample::camera_eye(c);
            const double along = -sample::dot(viewer, f) / sample::dot(d, f);
            const vgl_dvec3 q{
                viewer.x + d.x * along, viewer.y + d.y * along, viewer.z + d.z * along};
            const double a = 6378137.0;
            const double b = 6356752.314245;
            const double onto =
                1.0 / std::sqrt((q.x * q.x + q.y * q.y) / (a * a) + q.z * q.z / (b * b));
            const vgl_dvec3 rim{q.x * onto, q.y * onto, q.z * onto};
            // Along the top of the view, over space: the camera moves over the globe, still
            // looking the same way, and the point stays on the line from the centre to the
            // cursor, on the cursor's side.
            nav.press(drag::pan, 5.0, 5.0, c);
            double worst = 0.0;
            for (int k = 1; k <= 20; ++k)
            {
                const double mx = 5.0 + 10.0 * k;
                nav.move(mx, 5.0, c);
                double rx = 0.0;
                double ry = 0.0;
                if (!project(c, rim, w, h, rx, ry))
                {
                    worst = 180.0;
                    continue;
                }
                const double off = std::atan2((mx - cx) * (ry - cy) - (5.0 - cy) * (rx - cx),
                                              (mx - cx) * (rx - cx) + (5.0 - cy) * (ry - cy));
                worst = std::max(worst, std::abs(vgl::radians_to_degrees(off)));
            }
            const bool moved = std::abs(c.latitude - start.latitude) > 1e-6 ||
                               std::abs(c.longitude - start.longitude) > 1e-6;
            expect(moved && std::abs(c.altitude - start.altitude) < 1e-3 &&
                       std::abs(c.pitch - start.pitch) < 0.5,
                   "spinning from space carries the camera over the globe" + where);
            expect(worst < 0.05,
                   "spinning from space keeps the point on the line from the centre to the "
                   "cursor (" +
                       std::to_string(worst) + " degrees off)" + where);
            // Onto the globe at the centre: it grabs the point there and pans.
            nav.move(w / 2.0, h / 2.0, c);
            vgl_dvec3 held{};
            expect(nav.pick(c, w / 2.0, h / 2.0, held), "the centre is over the globe" + where);
            nav.move(w / 2.0 + 40.0, h / 2.0 - 30.0, c);
            double hx = 0.0;
            double hy = 0.0;
            expect(project(c, held, w, h, hx, hy) && near(hx, hy, w / 2.0 + 40.0, h / 2.0 - 30.0),
                   "a spin grabs the globe where the cursor reaches it" + where);
            // Off it again: spinning, at the height the pan left it.
            const sample::camera panned = c;
            nav.move(5.0, 5.0, c);
            nav.move(5.0, 60.0, c);
            nav.release();
            expect((std::abs(c.latitude - panned.latitude) > 1e-6 ||
                    std::abs(c.longitude - panned.longitude) > 1e-6) &&
                       std::abs(c.altitude - panned.altitude) < 1e-3,
                   "leaving the globe spins again" + where);
            nav.stop();

            // Pressed over the globe at the centre, then off it: the drag spins there too.
            c = start;
            nav.press(drag::pan, w / 2.0, h / 2.0, c);
            nav.move(5.0, 5.0, c);
            const sample::camera left = c;
            nav.move(60.0, 5.0, c);
            nav.release();
            expect((std::abs(c.latitude - left.latitude) > 1e-6 ||
                    std::abs(c.longitude - left.longitude) > 1e-6) &&
                       std::abs(c.altitude - left.altitude) < 1e-3 &&
                       std::abs(c.pitch - left.pitch) < 0.5,
                   "a pan that leaves the globe spins" + where);
            nav.stop();

            // Out into space and back: the spin keeps the grabbed point on the line from the
            // centre to the cursor, so that it comes back under the cursor.
            c = start;
            vgl_dvec3 taken{};
            expect(nav.pick(c, w / 2.0, h / 2.0, taken), "the centre is over the globe" + where);
            nav.press(drag::pan, w / 2.0, h / 2.0, c);
            const std::array<std::array<double, 2>, 6> path{{{w / 2.0 - 100.0, h / 2.0 - 60.0},
                                                             {5.0, 5.0},
                                                             {300.0, 5.0},
                                                             {700.0, 5.0},
                                                             {w / 2.0 + 30.0, h / 2.0 - 40.0},
                                                             {w / 2.0 + 10.0, h / 2.0 + 20.0}}};
            for (const std::array<double, 2>& m : path)
            {
                nav.move(m[0], m[1], c);
            }
            nav.release();
            double gx = 0.0;
            double gy = 0.0;
            expect(project(c, taken, w, h, gx, gy) && near(gx, gy, w / 2.0 + 10.0, h / 2.0 + 20.0),
                   "a point grabbed, taken out into space and back, is under the cursor (at " +
                       std::to_string(gx) + ", " + std::to_string(gy) + ")" + where);
            nav.stop();

            // Released while spinning, the globe carries on turning the same way, slowing down
            // until it stops: the point held on the disk keeps circling the centre's image.
            c = start;
            const auto around = [&](double& angle) {
                double rx = 0.0;
                double ry = 0.0;
                if (!project(c, rim, w, h, rx, ry))
                {
                    return false;
                }
                angle = std::atan2(ry - cy, rx - cx);
                return true;
            };
            nav.press(drag::pan, 5.0, 5.0, c);
            double before = 0.0;
            bool seen = around(before);
            for (int k = 1; k <= 20; ++k)
            {
                nav.move(5.0 + 10.0 * k, 5.0, c);
                nav.update(0.01, {}, c);
            }
            double released = 0.0;
            seen = around(released) && seen;
            nav.release();
            for (int k = 0; k < 10; ++k)
            {
                nav.update(0.01, {}, c);
            }
            double later = 0.0;
            seen = around(later) && seen;
            const double dragged = std::remainder(released - before, 2.0 * vgl::pi);
            const double thrown = std::remainder(later - released, 2.0 * vgl::pi);
            expect(seen && dragged * thrown > 0.0 && std::abs(c.altitude - start.altitude) < 1e-3,
                   "a spin released while moving throws the globe the same way" + where);
            for (int k = 0; k < 1000; ++k)
            {
                nav.update(0.01, {}, c);
            }
            const sample::camera settled = c;
            nav.update(0.01, {}, c);
            expect(c.latitude == settled.latitude && c.longitude == settled.longitude &&
                       c.heading == settled.heading,
                   "a thrown spin stops" + where);
        }

        // Looking straight down, orbiting sideways only turns the heading, steadily, around the
        // centre and around points off it, both ways: the camera keeps looking down.
        if (start.pitch >= 89.999)
        {
            for (const std::array<double, 2>& at :
                 std::array<std::array<double, 2>, 4>{{{w / 2.0, h / 2.0},
                                                       {w / 2.0 + 200.0, h / 2.0 - 120.0},
                                                       {300.0, 500.0},
                                                       {w / 2.0 + 1.0, h / 2.0}}})
            {
                for (const double step : {6.0, -6.0})
                {
                    c = start;
                    vgl_dvec3 pivot{};
                    if (!nav.pick(c, at[0], at[1], pivot))
                    {
                        continue; // Over space, where orbiting does nothing.
                    }
                    nav.press(drag::orbit, at[0], at[1], c);
                    double lowest = c.pitch;
                    double jump = 0.0;
                    for (int k = 1; k <= 120; ++k)
                    {
                        const double before = c.heading;
                        nav.move(at[0] + step * k, at[1], c);
                        lowest = std::min(lowest, c.pitch);
                        jump = std::max(jump, std::abs(std::remainder(c.heading - before, 360.0)));
                    }
                    nav.release();
                    double px = 0.0;
                    double py = 0.0;
                    const bool held = project(c, pivot, w, h, px, py) && near(px, py, at[0], at[1]);
                    expect(lowest > 89.0 && jump < 5.0 && held,
                           "orbiting sideways while looking straight down keeps looking down (" +
                               std::to_string(lowest) + " degrees at the least, heading jumps " +
                               std::to_string(jump) + " degrees, around " +
                               std::to_string(static_cast<int>(at[0])) + ", " +
                               std::to_string(static_cast<int>(at[1])) + ")" + where);
                    nav.stop();
                }
            }
        }

        // Pressed over space, the orbit does nothing.
        c = start;
        if (!nav.pick(c, 5.0, 5.0, sky))
        {
            nav.press(drag::orbit, 5.0, 5.0, c);
            for (int k = 1; k <= 20; ++k)
            {
                nav.move(5.0 + 4.0 * k, 5.0 + 3.0 * k, c);
            }
            nav.release();
            expect(c.latitude == start.latitude && c.longitude == start.longitude &&
                       c.altitude == start.altitude && c.heading == start.heading &&
                       c.pitch == start.pitch,
                   "orbiting from space does nothing" + where);
        }
    }

    // Flights to each place from the one before, and turns in place as R makes: smooth all the
    // way, starting at rest, and ending exactly at the place, at 60 frames a second.
    std::vector<std::pair<sample::camera, sample::camera>> flights;
    for (std::size_t i = 0; i < places.size(); ++i)
    {
        flights.emplace_back(places[(i + places.size() - 1) % places.size()].camera,
                             places[i].camera);
        sample::camera turned = places[i].camera;
        turned.heading = 0.0;
        turned.pitch = 90.0;
        flights.emplace_back(places[i].camera, turned);
    }
    for (const auto& [from, to] : flights)
    {
        sample::globe_navigation nav;
        nav.viewport(w, h, field_of_view);
        nav.ground(0.0);
        sample::camera c = from;
        nav.fly_to(c, to);
        // How far each frame moves the camera, in the terms the flight eases: the angle along
        // the great circle, the altitude on a logarithmic scale, and the turn of the view. A
        // jump would show as one frame taking much more of any of them than its share.
        const auto direction = [](const sample::camera& k) {
            const double lat = vgl::degrees_to_radians(k.latitude);
            const double lon = vgl::degrees_to_radians(k.longitude);
            return std::array<double, 3>{
                std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
        };
        const auto step = [&](const sample::camera& p, const sample::camera& q) {
            const std::array<double, 3> u = direction(p);
            const std::array<double, 3> v = direction(q);
            const double d = std::clamp(u[0] * v[0] + u[1] * v[1] + u[2] * v[2], -1.0, 1.0);
            return std::array<double, 3>{
                std::acos(d),
                std::fabs(std::log(q.altitude / p.altitude)),
                std::max(std::fabs(std::remainder(q.heading - p.heading, 360.0)),
                         std::fabs(q.pitch - p.pitch))};
        };
        std::array<double, 3> total{};
        std::array<double, 3> largest{};
        std::array<double, 3> first{-1.0, -1.0, -1.0};
        int frames = 0;
        while (nav.flying() && frames < 1000)
        {
            const sample::camera before = c;
            nav.update(1.0 / 60.0, {}, c);
            const std::array<double, 3> d = step(before, c);
            for (std::size_t k = 0; k < 3; ++k)
            {
                total[k] += d[k];
                largest[k] = std::max(largest[k], d[k]);
            }
            if (first[0] < 0.0)
            {
                first = d;
            }
            ++frames;
        }
        const std::string what =
            " flying " + std::to_string(static_cast<int>(step(from, to)[0] * 6371.0)) +
            " km and turning " + std::to_string(static_cast<int>(step(from, to)[2])) + " degrees";
        expect(!nav.flying() && c.latitude == to.latitude && c.longitude == to.longitude &&
                   c.altitude == to.altitude && c.heading == to.heading && c.pitch == to.pitch,
               "a flight ends at its place" + what);
        expect(frames >= 25 && frames <= 250,
               "a flight takes half a second to four seconds" + what);
        // An s-curve's fastest frame covers 1.875 / frames of the way; the altitude's, with the
        // rise over long flights on top, up to about 2.5 / frames. A jump takes far more.
        const std::array<double, 3> share = {
            2.0 / std::max(frames, 1), 3.0 / std::max(frames, 1), 2.0 / std::max(frames, 1)};
        bool smooth = true;
        bool at_rest = true;
        for (std::size_t k = 0; k < 3; ++k)
        {
            smooth = smooth && largest[k] <= share[k] * total[k] + 1e-9;
            at_rest = at_rest && first[k] <= 0.002 * total[k] + 1e-9;
        }
        expect(smooth, "a flight moves smoothly, without a jump" + what);
        expect(at_rest, "a flight starts at rest" + what);
    }
}

struct options
{
    std::string data;               // The sample set's directory, from --data.
    std::vector<std::string> packs; // From --pack.
    std::string cache;              // From --cache.
    std::uint64_t cache_budget = 0; // From --cache-budget, in bytes; 0 for the default.
    std::string stars;              // A star catalogue, from --stars.
    bool es = false;
    bool egl = false;
    bool check = false;
    std::string save;
    vgl_depth_convention depth = VGL_DEPTH_CONVENTION_LOGARITHMIC;
};

int run_check(const options& o, const host_gl& gl, vgl::context& ctx)
{
    constexpr int width = 640;
    constexpr int height = 360;
    navigation_check();
    GLuint host_vertex_array = 0;
    gl.GenVertexArrays(1, &host_vertex_array);

    const std::array<vgl_depth_convention, 3> conventions{VGL_DEPTH_CONVENTION_LOGARITHMIC,
                                                          VGL_DEPTH_CONVENTION_REVERSED_Z,
                                                          VGL_DEPTH_CONVENTION_CONVENTIONAL};
    std::vector<std::vector<image>> images(conventions.size());
    std::vector<std::vector<image>> atmosphere_images(conventions.size());
    for (std::size_t c = 0; c < conventions.size(); ++c)
    {
        scene s;
        s.depth = conventions[c];
        if (!depth_mode_set(gl, s.depth))
        {
            std::printf("%-13s skipped: the device has no clip control\n", depth_name(s.depth));
            continue;
        }
        auto info = vgl::init<vgl_gl_backend_create_info>();
        info.depth_convention = s.depth;
        info.get_proc_address = get_proc_address;
        vgl::expected<vgl::gl_backend> backend = vgl::gl_backend::create(ctx, info);
        if (!backend)
        {
            const bool unsupported = backend.error() == vgl::result::not_supported &&
                                     s.depth == VGL_DEPTH_CONVENTION_REVERSED_Z;
            std::printf("%-13s %s\n",
                        depth_name(s.depth),
                        unsupported ? "skipped: not supported" : "failed");
            expect(unsupported, std::string("the backend is created for ") + depth_name(s.depth));
            continue;
        }
        const std::vector<vgl_star> stars = sample::sky_stars();
        expect(backend->stars_set(stars) == vgl::result::success, "the stars are set");
        // An image too small to cover the globe is refused.
        const std::uint8_t pixel = 0;
        expect(backend->clouds_set(&pixel, 1, 1) == vgl::result::invalid_argument,
               "a cloud image of one pixel is refused");
        const sample::cloud_image& clouds = sample::sky_clouds();
        expect(clouds.width == 0 ||
                   backend->clouds_set(clouds.coverage.data(), clouds.width, clouds.height) ==
                       vgl::result::success,
               "the clouds are set");
        // A star without a direction is refused, and the stars stay as they were.
        const vgl_star none{};
        expect(backend->stars_set({&none, 1}) == vgl::result::invalid_argument,
               "an invalid star is refused");
        target t;
        expect(target_create(gl, t, width, height, s.depth == VGL_DEPTH_CONVENTION_REVERSED_Z),
               "the framebuffer is complete");

        for (const place& p : places)
        {
            s.camera = p.camera;
            int frames = 0;
            int quiet = 0;
            while (frames < 3000 && quiet < 5)
            {
                view_set(ctx, s, width, height);
                expect(ctx.update() == vgl::result::success, "vgl_context_update succeeds");
                clear(gl, s, t);
                sentinel_state_set(gl, host_vertex_array);
                expect(draw(*backend, s, width, height) == vgl::result::success,
                       "vgl_gl_backend_draw succeeds");
                sentinel_state_check(gl, host_vertex_array, s.depth);
                const vgl_context_statistics st = ctx.statistics_get();
                quiet = sample::idle(st) ? quiet + 1 : 0;
                ++frames;
                glfwPollEvents();
                if (quiet == 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            const vgl_context_statistics st = ctx.statistics_get();
            image pixels = read_image(gl, t);
            sample::check_place(s.depth, p, frames, quiet >= 5, st, pixels);
            expect(gl.GetError() == 0,
                   std::string("OpenGL reports no error: ") + depth_name(s.depth) + ", " + p.name);
            if (!o.save.empty())
            {
                sample::save_image(pixels, sample::image_path(o.save, s.depth, p.name));
                // The same view as elevation: colours by height, and contours at the interval
                // the interactive sample would choose.
                scene elevation = s;
                elevation.debug.height_colors = true;
                elevation.debug.contours = true;
                elevation.debug.contour_interval = contour_interval_for(s.camera.altitude);
                clear(gl, elevation, t);
                static_cast<void>(draw(*backend, elevation, width, height));
                sample::save_image(
                    read_image(gl, t),
                    sample::image_path(
                        o.save, s.depth, (std::string(p.name) + "-elevation").c_str()));
                // And with the sea surface over the imagery.
                scene sea = s;
                sea.debug.sea = true;
                clear(gl, sea, t);
                static_cast<void>(draw(*backend, sea, width, height));
                sample::save_image(
                    read_image(gl, t),
                    sample::image_path(o.save, s.depth, (std::string(p.name) + "-sea").c_str()));
            }
            images[c].push_back(std::move(pixels));

            // With the atmosphere: the sky where nothing was drawn, and the haze, the same with
            // every convention, and the host's state as it was.
            scene air = s;
            air.debug.atmosphere = true;
            air.debug.stars = true;
            air.debug.clouds = true;
            clear(gl, air, t);
            sentinel_state_set(gl, host_vertex_array);
            expect(draw(*backend, air, width, height) == vgl::result::success,
                   "vgl_gl_backend_draw succeeds with the atmosphere");
            sentinel_state_check(gl, host_vertex_array, s.depth);
            expect(gl.GetError() == 0,
                   std::string("OpenGL reports no error with the atmosphere: ") +
                       depth_name(s.depth) + ", " + p.name);
            image sky = read_image(gl, t);
            if (!o.save.empty())
            {
                sample::save_image(
                    sky,
                    sample::image_path(
                        o.save, s.depth, (std::string(p.name) + "-atmosphere").c_str()));
            }
            atmosphere_images[c].push_back(std::move(sky));
        }
        // The debug views draw without OpenGL errors, and leave the host's state as it was.
        s.debug.wireframe = s.debug.level_colors = s.debug.imagery_source = true;
        s.debug.height_colors = s.debug.contours = s.debug.sea = true;
        clear(gl, s, t);
        sentinel_state_set(gl, host_vertex_array);
        expect(draw(*backend, s, width, height) == vgl::result::success,
               "vgl_gl_backend_draw succeeds with the debug views");
        sentinel_state_check(gl, host_vertex_array, s.depth);
        expect(gl.GetError() == 0, "OpenGL reports no error with the debug views");
        if (!o.save.empty())
        {
            sample::save_image(read_image(gl, t), sample::image_path(o.save, s.depth, "debug"));
        }
        // The wireframe alone: the grid's cells and the tiles' boundaries over the imagery.
        s.debug.level_colors = s.debug.imagery_source = false;
        s.debug.height_colors = s.debug.contours = s.debug.sea = false;
        clear(gl, s, t);
        expect(draw(*backend, s, width, height) == vgl::result::success,
               "vgl_gl_backend_draw succeeds with the wireframe");
        if (!o.save.empty())
        {
            sample::save_image(read_image(gl, t), sample::image_path(o.save, s.depth, "wireframe"));
        }
        s.debug.height_colors = s.debug.contours = s.debug.sea = true;
        // The heightmap with contour lines alone, every 100 m.
        s.debug.wireframe = s.debug.level_colors = s.debug.imagery_source = false;
        s.debug.contour_interval = 100.0f;
        clear(gl, s, t);
        expect(draw(*backend, s, width, height) == vgl::result::success,
               "vgl_gl_backend_draw succeeds with contour lines");
        expect(gl.GetError() == 0, "OpenGL reports no error with contour lines");
        if (!o.save.empty())
        {
            sample::save_image(read_image(gl, t), sample::image_path(o.save, s.depth, "contours"));
        }
        target_destroy(gl, t);
        backend->reset();
    }
    depth_mode_set(gl, VGL_DEPTH_CONVENTION_LOGARITHMIC);
    gl.DeleteVertexArrays(1, &host_vertex_array);

    sample::compare_conventions({conventions.begin(), conventions.end()}, images);
    sample::compare_conventions({conventions.begin(), conventions.end()}, atmosphere_images);
    std::printf("\n%s\n", sample::failures() == 0 ? "all checks passed" : "checks failed");
    return sample::failures() == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------
// Interactive
// ---------------------------------------------------------------------------------------------

int run_interactive(const options& o,
                    GLFWwindow* window,
                    const host_gl& gl,
                    vgl::context& ctx,
                    const std::vector<sample::layer_control>& layers,
                    const sample::pack_reader& reader)
{
    scene s;
    s.debug.atmosphere = true;
    s.debug.stars = true;
    s.debug.clouds = true;
    s.depth = o.depth;
    s.camera = places[0].camera;
    if (!depth_mode_set(gl, s.depth))
    {
        std::fprintf(stderr,
                     "the reversed-Z convention needs clip control, which this device lacks\n");
        return 1;
    }
    auto info = vgl::init<vgl_gl_backend_create_info>();
    info.depth_convention = s.depth;
    info.get_proc_address = get_proc_address;
    vgl::expected<vgl::gl_backend> backend = vgl::gl_backend::create(ctx, info);
    if (!backend)
    {
        std::fprintf(
            stderr, "could not create the backend: %s\n", vgl::result_name_get(backend.error()));
        return 1;
    }
    static_cast<void>(backend->stars_set(sample::sky_stars()));
    if (const sample::cloud_image& clouds = sample::sky_clouds(); clouds.width != 0)
    {
        static_cast<void>(backend->clouds_set(clouds.coverage.data(), clouds.width, clouds.height));
    }

    // The window's callbacks first: Dear ImGui chains to them.
    sample::overlay ui;
    sample::globe_navigation nav;
    nav.terrain(ctx.get()); // Grab the terrain under the cursor.
    sample::glfw_controls controls;
    controls.camera = &s.camera;
    controls.debug = &s.debug;
    controls.ui = &ui;
    controls.nav = &nav;
    sample::controls_install(window, &controls);
    struct imgui_host
    {
        GLFWwindow* window;
        bool es;
    } host{window, o.es};
    sample::overlay::backend imgui;
    imgui.user_data = &host;
    imgui.init = [](void* user) {
        const auto* h = static_cast<const imgui_host*>(user);
        return ImGui_ImplGlfw_InitForOpenGL(h->window, true) &&
               ImGui_ImplOpenGL3_Init(h->es ? "#version 300 es" : "#version 330 core");
    };
    imgui.new_frame = [](void*) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
    };
    imgui.render = [](void*) { ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); };
    imgui.shutdown = [](void*) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    };
    if (!ui.init(imgui))
    {
        std::fprintf(stderr, "could not set up the overlay\n");
        return 1;
    }
    sample::panel_info panel;
    panel.layers = layers;
    panel.api = reinterpret_cast<const char*>(gl.GetString(GL_VERSION));
    panel.device = reinterpret_cast<const char*>(gl.GetString(GL_RENDERER));
    panel.depth = depth_name(s.depth);
    bool was_frozen = false;
    auto last_frame = std::chrono::steady_clock::now();
    auto last_navigated = last_frame;

    target t;
    auto last_title = std::chrono::steady_clock::now();
    int frames = 0;
    // One frame: the native loop below calls it, and in a browser the page does.
    auto frame = [&] {
        glfwPollEvents();
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        if (width == 0 || height == 0)
        {
            glfwWaitEvents(); // Minimized.
            return;
        }
        if (width != t.width || height != t.height)
        {
            if (t.framebuffer != 0)
            {
                target_destroy(gl, t);
            }
            target_create(gl, t, width, height, s.depth == VGL_DEPTH_CONVENTION_REVERSED_Z);
        }

        // Navigation: throws, smooth zooms and keys, over the terrain under the camera.
        const vgl::expected<vgl::height_sample> ground =
            ctx.height_query(vgl::degrees_to_radians(s.camera.latitude),
                             vgl::degrees_to_radians(s.camera.longitude));
        if (ground && ground->status == vgl::result::success)
        {
            nav.ground(ground->height);
            s.camera.altitude = std::max(s.camera.altitude, ground->height + 20.0);
        }
        int window_width = 0;
        int window_height = 0;
        glfwGetWindowSize(window, &window_width, &window_height);
        nav.viewport(window_width, window_height, field_of_view);
        const sample::navigation_keys keys = sample::controls_keys(window, controls);
        // From one update to the next, so that the time spent updating and drawing counts too:
        // movements then keep their speed however long frames take.
        const auto navigated = std::chrono::steady_clock::now();
        nav.update(
            std::chrono::duration<double>(navigated - last_navigated).count(), keys, s.camera);
        last_navigated = navigated;

        // Contours at an interval that follows the altitude.
        if (s.debug.contour_auto)
        {
            s.debug.contour_interval = contour_interval_for(s.camera.altitude);
        }

        // Freezing keeps the camera of that moment for selection.
        if (s.debug.freeze && !was_frozen)
        {
            s.frozen = s.camera;
        }
        was_frozen = s.debug.freeze;

        view_set(ctx, s, width, height);
        static_cast<void>(ctx.update());
        clear(gl, s, t);
        static_cast<void>(draw(*backend, s, width, height));

        // Copy the frame to the window, and draw the overlay over it.
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, t.framebuffer);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        gl.BlitFramebuffer(
            0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        const auto frame_end = std::chrono::steady_clock::now();
        const double frame_seconds = std::chrono::duration<double>(frame_end - last_frame).count();
        last_frame = frame_end;
        const double aspect = static_cast<double>(width) / height;
        panel.live = s.camera;
        panel.frozen = s.frozen;
        panel.focus_strength = 0.0;
        double focus_strength = 0.0;
        if (nav.focus(s.camera, panel.focus_x, panel.focus_y, focus_strength))
        {
            panel.focus_strength = focus_strength;
        }
        panel.credits = ctx.attribution_get();
        panel.cache = reader.cache_info();
        // The frozen frustum reaches to about the frozen camera's horizon.
        const double reach = std::sqrt(s.frozen.altitude * (2.0 * 6371000.0 + s.frozen.altitude));
        ui.frame(ctx.statistics_get(),
                 panel,
                 frame_seconds,
                 sample::camera_view_matrix(s.camera),
                 projection_for(s, s.camera, aspect),
                 sample::frustum_of(s.frozen, field_of_view, aspect, reach),
                 s.debug);
        ui.render();
#if !defined(__EMSCRIPTEN__)
        glfwSwapBuffers(window); // A browser shows the canvas when the frame returns.
#endif

        ++frames;
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - last_title).count();
        if (seconds >= 0.5)
        {
            const vgl_context_statistics st = ctx.statistics_get();
            std::array<char, 256> title{};
            std::snprintf(title.data(),
                          title.size(),
                          "vgl - %s depth - %.0f fps - %u tiles - %.0f MiB GPU - %.0f km",
                          depth_name(s.depth),
                          frames / seconds,
                          st.tiles_drawn,
                          static_cast<double>(st.gpu_used) / (1024.0 * 1024.0),
                          s.camera.altitude / 1000.0);
            glfwSetWindowTitle(window, title.data());
            frames = 0;
            last_title = now;
        }
    };
#if defined(__EMSCRIPTEN__)
    // The browser calls the frames, between which the page must return to it. Simulating an
    // endless loop ends this call without unwinding its stack, so the state above stays where
    // the frames find it, and nothing after this line runs.
    emscripten_set_main_loop_arg(
        [](void* f) { (*static_cast<decltype(frame)*>(f))(); }, &frame, 0, true);
#else
    while (glfwWindowShouldClose(window) == 0)
    {
        frame();
    }
#endif
    if (t.framebuffer != 0)
    {
        target_destroy(gl, t);
    }
    // The credits of what was in view last, as a host would show them.
    std::printf("credits:\n");
    for (const char* line : ctx.attribution_get())
    {
        std::printf("  %s\n", line);
    }
    return 0;
}

bool parse(int argc, char** argv, options& o)
{
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (std::strcmp(a, "--check") == 0)
        {
            o.check = true;
        }
        else if (std::strcmp(a, "--es") == 0)
        {
            o.es = true;
        }
        else if (std::strcmp(a, "--egl") == 0)
        {
            o.egl = true;
        }
        else if (std::strcmp(a, "--data") == 0 && i + 1 < argc)
        {
            o.data = argv[++i];
        }
        else if (std::strcmp(a, "--stars") == 0 && i + 1 < argc)
        {
            o.stars = argv[++i];
        }
        else if (std::strcmp(a, "--pack") == 0 && i + 1 < argc)
        {
            o.packs.emplace_back(argv[++i]);
        }
        else if (std::strcmp(a, "--cache") == 0 && i + 1 < argc)
        {
            o.cache = argv[++i];
        }
        else if (std::strcmp(a, "--cache-budget") == 0 && i + 1 < argc)
        {
            if (!sample::parse_cache_budget(argv[++i], o.cache_budget))
            {
                return false;
            }
        }
        else if (std::strcmp(a, "--save") == 0 && i + 1 < argc)
        {
            o.save = argv[++i];
        }
        else if (std::strcmp(a, "--depth") == 0 && i + 1 < argc)
        {
            const char* d = argv[++i];
            if (std::strcmp(d, "logarithmic") == 0)
            {
                o.depth = VGL_DEPTH_CONVENTION_LOGARITHMIC;
            }
            else if (std::strcmp(d, "reversed") == 0)
            {
                o.depth = VGL_DEPTH_CONVENTION_REVERSED_Z;
            }
            else if (std::strcmp(d, "conventional") == 0)
            {
                o.depth = VGL_DEPTH_CONVENTION_CONVENTIONAL;
            }
            else
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }
#if defined(__EMSCRIPTEN__)
    if (o.check)
    {
        return false; // The check waits in loops, which a page cannot.
    }
#endif
    // The check expects the sample set, and --pack replaces it.
    return o.packs.empty() || (o.data.empty() && !o.check);
}

} // namespace

int main(int argc, char** argv)
{
    options o;
    if (!parse(argc, argv, o))
    {
        std::fprintf(stderr,
                     "usage: vgl-gl [--data <directory> | --pack <uri>...] [--cache <directory> "
                     "[--cache-budget <MiB>]] "
                     "[--es] [--egl] "
                     "[--depth logarithmic|reversed|conventional] [--check] "
                     "[--save <directory>] [--stars <catalog>]\n"
                     "--cache keeps what comes from web servers in a directory, for next time "
                     "and offline, within --cache-budget mebibytes (1024 by default). "
                     "--pack, repeated, replaces the sample set, and cannot be combined with "
                     "--data or --check. Without either, the sample set is read from "
                     "$%s, or else from %s. --stars draws the stars of the Yale Bright Star "
                     "Catalogue's file \"catalog\" (CDS V/50, decompressed) instead of "
                     "generated ones.\n",
                     sample::data_variable,
                     VGL_SAMPLE_DATA_DIR);
        return 2;
    }
    if (!o.stars.empty())
    {
        std::string error;
        if (!sample::stars_load(o.stars, error))
        {
            std::fprintf(stderr, "--stars: %s\n", error.c_str());
            return 1;
        }
    }
    {
        // The clouds of the sample set, if it has them.
        std::string error;
        if (!sample::clouds_load(sample::data_directory(o.data, VGL_SAMPLE_DATA_DIR), error))
        {
            std::fprintf(stderr, "no clouds: %s\n", error.c_str());
        }
    }

    glfwSetErrorCallback(
        [](int, const char* message) { std::fprintf(stderr, "glfw: %s\n", message); });
    if (glfwInit() == 0)
    {
        return 1;
    }
#if defined(__EMSCRIPTEN__)
    o.es = true;                                       // WebGL 2.
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE); // At the screen's pixel ratio.
#endif
    if (o.es)
    {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    }
    else
    {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE); // macOS needs it.
    }
    if (o.egl)
    {
        glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
    }
    if (o.check)
    {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }
    GLFWwindow* window =
        glfwCreateWindow(o.check ? 640 : 1280, o.check ? 360 : 720, "vgl", nullptr, nullptr);
    if (window == nullptr)
    {
        glfwTerminate();
        return 1;
    }
#if defined(__EMSCRIPTEN__)
    static_cast<void>(emscripten::glfw3::MakeCanvasResizable(window, "window"));
#endif
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    const host_gl gl = load_host_gl();
    std::printf("%s\n%s\n\n",
                reinterpret_cast<const char*>(gl.GetString(GL_VERSION)),
                reinterpret_cast<const char*>(gl.GetString(GL_RENDERER)));

    // The defaults: a thread pool for decoding and reading, and a file reader on it, with an
    // HTTP reader in front when vgl has one.
    vgl::expected<vgl::thread_pool> pool = vgl::thread_pool::create();
    if (!pool)
    {
        std::fprintf(stderr, "could not create the thread pool\n");
        return 1;
    }
    vgl::expected<sample::pack_reader> reader =
        sample::pack_reader::create(*pool, o.cache, o.cache_budget);
    if (!reader)
    {
        std::fprintf(stderr, "could not create the file reader\n");
        return 1;
    }
    const vgl_task_interface tasks = pool->task_interface_get();
    const vgl_io_interface io = reader->io_interface_get();
    auto info = vgl::init<vgl_context_create_info>();
    info.tasks = &tasks;
    info.io = &io;
    info.log_level = VGL_LOG_LEVEL_INFO;
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
    const std::vector<sample::pack_source> sources =
        o.packs.empty() ? sample::sample_packs(sample::data_directory(o.data, VGL_SAMPLE_DATA_DIR))
                        : sample::named_packs(o.packs);
    reader->warn_unreadable(sources);
    std::vector<vgl::pack> packs;
    for (const sample::pack_source& s : sources)
    {
        std::printf("pack %s\n", s.uri.c_str());
        vgl::expected<vgl::pack> p = vgl::pack::create(*ctx, s.uri.c_str(), s.priority);
        if (!p)
        {
            std::fprintf(stderr,
                         "could not add the pack %s: %s\n",
                         s.uri.c_str(),
                         vgl::result_name_get(p.error()));
            return 1;
        }
        packs.push_back(std::move(*p));
    }
    std::printf("\n");

    const int code =
        o.check
            ? run_check(o, gl, *ctx)
            : run_interactive(o, window, gl, *ctx, sample::layer_controls(sources, packs), *reader);

    // Leave in order: the backend went with its run; then packs and context, the reader and
    // the pool, and last the window with its OpenGL context.
    packs.clear();
    ctx->reset();
    reader->reset();
    pool->reset();
    glfwDestroyWindow(window);
    glfwTerminate();
    return code;
}
