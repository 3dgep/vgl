// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// OpenGL sample (design document, section 11): the globe drawn by the OpenGL backend in a GLFW
// window, on OpenGL 3.3 or, with --es, on OpenGL ES 3.0. The host owns the window, the context,
// the camera and the framebuffer; vgl draws into it.
//
//   vgl-gl [--data <directory>] [--es] [--egl] [--depth logarithmic|reversed|conventional]
//          [--check] [--save <directory>]
//
// Controls: drag with the left button to move, with the right button to turn and tilt, scroll
// to climb or descend; 1 to 5 jump to places; L switches between sunlight and a headlight.
//
// With --check, the sample runs hidden: it flies to a few views with each depth convention
// the device supports, and checks what it draws, that the conventions agree, that OpenGL
// reports no errors and that the backend restores the host's state. It exits with 1 on a
// failure, so that it doubles as a test of the backend. --save writes the images it checks as
// PPM files.

#define GLFW_INCLUDE_NONE
#include "sample_camera.hpp"

#include <vgl/defaults.hpp>
#include <vgl/gl.hpp>
#include <vgl/vgl.hpp>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef VGL_SAMPLE_DATA_DIR
#    define VGL_SAMPLE_DATA_DIR "."
#endif

namespace
{

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

constexpr double field_of_view = 60.0; // Degrees, vertical.
constexpr std::array<float, 3> clear_color{0.02f, 0.02f, 0.05f};

struct scene
{
    vgl_depth_convention depth = VGL_DEPTH_CONVENTION_LOGARITHMIC;
    sample::camera camera;
    bool sunlight = true;
};

vgl_dmat4 projection_for(const scene& s, double aspect)
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
            fovy, aspect, std::max(1.0, s.camera.altitude * 0.05), s.camera.altitude + 2.0e7);
    default:
        // The logarithmic convention writes its own depth; the planes do not matter.
        return sample::perspective(fovy, aspect, 1.0, 1.0e9);
    }
}

// Sets the view for the next update, from the scene's camera.
void view_set(vgl::context& ctx, const scene& s, int width, int height)
{
    auto view = vgl::init<vgl_view>();
    view.view_matrix = sample::camera_view_matrix(s.camera);
    view.projection_matrix = projection_for(s, static_cast<double>(width) / height);
    view.viewport_width = static_cast<std::uint32_t>(width);
    view.viewport_height = static_cast<std::uint32_t>(height);
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

// Has the backend draw the globe into the bound framebuffer.
vgl::result draw(vgl::gl_backend& backend, const scene& s)
{
    auto info = vgl::init<vgl_gl_draw_info>();
    if (s.sunlight)
    {
        info.sun_direction = {1.0, 0.4, 0.3}; // Over the Atlantic, a little north.
    }
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

const char* depth_name(vgl_depth_convention depth)
{
    switch (depth)
    {
    case VGL_DEPTH_CONVENTION_REVERSED_Z:
        return "reversed-Z";
    case VGL_DEPTH_CONVENTION_CONVENTIONAL:
        return "conventional";
    default:
        return "logarithmic";
    }
}

// ---------------------------------------------------------------------------------------------
// Places
// ---------------------------------------------------------------------------------------------

// With the share of a 16:9 view the globe covers there, for the check: from orbit, the globe
// is a disc in the middle; closer, the terrain fills the view below the horizon.
struct place
{
    const char* name;
    sample::camera camera;
    double least_coverage;
    double most_coverage;
};

const std::array<place, 5> places{{
    {"orbit, Atlantic", {30.0, -30.0, 2.0e7, 0.0, 90.0}, 0.06, 0.12},
    {"Europe", {48.0, 10.0, 5.0e6, 0.0, 90.0}, 0.45, 0.70},
    {"Alps", {44.5, 9.0, 3.0e5, 0.0, 35.0}, 0.65, 0.82},
    {"Himalaya", {26.0, 86.9, 6.0e4, 0.0, 20.0}, 0.58, 0.77},
    {"Mid-Atlantic Ridge", {20.0, -50.0, 1.5e6, 90.0, 40.0}, 0.38, 0.55},
}};

// ---------------------------------------------------------------------------------------------
// The check
// ---------------------------------------------------------------------------------------------

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::fprintf(stderr, "check failed: %s\n", what.c_str());
        ++failures;
    }
}

// Host state set before each draw in the check, which the backend must leave as it was.
void sentinel_state_set(const host_gl& gl, GLuint vertex_array)
{
    gl.BindVertexArray(vertex_array);
    gl.Enable(GL_BLEND);
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

using image = std::vector<std::uint8_t>; // RGBA, bottom row first.

image read_image(const host_gl& gl, const target& t)
{
    image pixels(static_cast<std::size_t>(t.width) * static_cast<std::size_t>(t.height) * 4);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, t.framebuffer);
    gl.ReadPixels(0, 0, t.width, t.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    return pixels;
}

void save_image(const image& pixels, int width, int height, const std::string& path)
{
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y)
    {
        for (int x = 0; x < width; ++x)
        {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(x);
            const auto* p = &pixels[i * 4];
            f.write(reinterpret_cast<const char*>(p), 3);
        }
    }
    if (!f)
    {
        std::fprintf(stderr, "could not write %s\n", path.c_str());
    }
}

// Whether a pixel shows the globe rather than the clear color.
bool covered_pixel(const image& pixels, std::size_t pixel)
{
    for (std::size_t c = 0; c < 3; ++c)
    {
        const int background = static_cast<int>(std::lround(clear_color[c] * 255.0f));
        if (std::abs(pixels[pixel * 4 + c] - background) > 2)
        {
            return true;
        }
    }
    return false;
}

// The share of pixels that show the globe.
double coverage(const image& pixels)
{
    const std::size_t count = pixels.size() / 4;
    std::size_t n = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (covered_pixel(pixels, i))
        {
            ++n;
        }
    }
    return static_cast<double>(n) / static_cast<double>(count);
}

// The share of pixels whose color differs by more than a little.
double difference(const image& a, const image& b)
{
    std::size_t differing = 0;
    for (std::size_t i = 0; i < a.size(); i += 4)
    {
        const int d = std::max(
            {std::abs(a[i] - b[i]), std::abs(a[i + 1] - b[i + 1]), std::abs(a[i + 2] - b[i + 2])});
        if (d > 24)
        {
            ++differing;
        }
    }
    return static_cast<double>(differing) / static_cast<double>(a.size() / 4);
}

bool idle(const vgl_context_statistics& s)
{
    return s.packs_loading == 0 && s.reads_outstanding == 0 && s.tiles_decoding == 0 &&
           s.meshes_building == 0 && s.meshes_waiting == 0 && s.resources_created == 0 &&
           s.tiles_drawn == s.tiles_selected;
}

struct options
{
    std::string data = VGL_SAMPLE_DATA_DIR;
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
    GLuint host_vertex_array = 0;
    gl.GenVertexArrays(1, &host_vertex_array);

    const std::array<vgl_depth_convention, 3> conventions{VGL_DEPTH_CONVENTION_LOGARITHMIC,
                                                          VGL_DEPTH_CONVENTION_REVERSED_Z,
                                                          VGL_DEPTH_CONVENTION_CONVENTIONAL};
    std::vector<std::vector<image>> images(conventions.size());
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
                expect(draw(*backend, s) == vgl::result::success, "vgl_gl_backend_draw succeeds");
                sentinel_state_check(gl, host_vertex_array, s.depth);
                const vgl_context_statistics st = ctx.statistics_get();
                quiet = idle(st) ? quiet + 1 : 0;
                ++frames;
                glfwPollEvents();
                if (quiet == 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            const vgl_context_statistics st = ctx.statistics_get();
            image pixels = read_image(gl, t);
            const double covered = coverage(pixels);
            std::printf("%-13s %-20s %5d frames %5u tiles %6.1f MiB  %5.1f%% globe\n",
                        depth_name(s.depth),
                        p.name,
                        frames,
                        st.tiles_drawn,
                        static_cast<double>(st.gpu_used) / (1024.0 * 1024.0),
                        covered * 100.0);
            const std::string where = std::string(depth_name(s.depth)) + ", " + p.name;
            expect(quiet >= 5, "loading settles: " + where);
            expect(st.tiles_drawn > 0 && st.tiles_drawn == st.tiles_selected,
                   "every selected tile is drawn: " + where);
            expect(gl.GetError() == 0, "OpenGL reports no error: " + where);
            expect(covered >= p.least_coverage && covered <= p.most_coverage,
                   "the globe covers its share of the view: " + where);
            // Every view looks at the globe in the middle, and past it in the top corners.
            const std::size_t w = width;
            const std::size_t h = height;
            expect(covered_pixel(pixels, h / 2 * w + w / 2) &&
                       !covered_pixel(pixels, (h - 1) * w) && !covered_pixel(pixels, h * w - 1),
                   "the globe is below and the sky above: " + where);
            if (!o.save.empty())
            {
                std::string name = o.save + "/" + depth_name(s.depth) + "-" + p.name + ".ppm";
                std::replace(name.begin() + static_cast<std::ptrdiff_t>(o.save.size()),
                             name.end(),
                             ' ',
                             '-');
                std::replace(name.begin() + static_cast<std::ptrdiff_t>(o.save.size()),
                             name.end(),
                             ',',
                             '_');
                save_image(pixels, width, height, name);
            }
            images[c].push_back(std::move(pixels));
        }
        target_destroy(gl, t);
        backend->reset();
    }
    depth_mode_set(gl, VGL_DEPTH_CONVENTION_LOGARITHMIC);
    gl.DeleteVertexArrays(1, &host_vertex_array);

    // Where the conventions resolve depth well, they draw the same image. A conventional depth
    // buffer loses distant detail, so it is held to a looser bound.
    for (std::size_t c = 1; c < conventions.size(); ++c)
    {
        if (images[0].size() != places.size() || images[c].size() != places.size())
        {
            continue;
        }
        for (std::size_t i = 0; i < places.size(); ++i)
        {
            const double d = difference(images[0][i], images[c][i]);
            const double bound = conventions[c] == VGL_DEPTH_CONVENTION_CONVENTIONAL ? 0.05 : 0.01;
            std::printf("%s and %s differ in %.2f%% of %s\n",
                        depth_name(conventions[0]),
                        depth_name(conventions[c]),
                        d * 100.0,
                        places[i].name);
            expect(d <= bound,
                   std::string(depth_name(conventions[c])) +
                       " agrees with logarithmic depth: " + places[i].name);
        }
    }
    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "checks failed");
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------
// Interactive
// ---------------------------------------------------------------------------------------------

struct input
{
    scene* s = nullptr;
    bool dragging_left = false;
    bool dragging_right = false;
    double x = 0.0;
    double y = 0.0;
};

void on_mouse_button(GLFWwindow* window, int button, int action, int /*mods*/)
{
    auto* in = static_cast<input*>(glfwGetWindowUserPointer(window));
    const bool down = action == GLFW_PRESS;
    if (button == GLFW_MOUSE_BUTTON_LEFT)
    {
        in->dragging_left = down;
    }
    else if (button == GLFW_MOUSE_BUTTON_RIGHT)
    {
        in->dragging_right = down;
    }
    glfwGetCursorPos(window, &in->x, &in->y);
}

void on_cursor(GLFWwindow* window, double x, double y)
{
    auto* in = static_cast<input*>(glfwGetWindowUserPointer(window));
    const double dx = x - in->x;
    const double dy = y - in->y;
    in->x = x;
    in->y = y;
    sample::camera& c = in->s->camera;
    if (in->dragging_left)
    {
        // About the ground distance a pixel spans, turned into degrees.
        int width = 0;
        int height = 0;
        glfwGetWindowSize(window, &width, &height);
        const double metres = c.altitude * 1.15 / std::max(height, 1);
        const double degrees = metres / 111320.0;
        const double h = vgl::degrees_to_radians(c.heading);
        const double forward = dy * degrees;
        const double right = -dx * degrees;
        c.latitude =
            std::clamp(c.latitude + forward * std::cos(h) - right * std::sin(h), -89.0, 89.0);
        c.longitude += (forward * std::sin(h) + right * std::cos(h)) /
                       std::max(std::cos(vgl::degrees_to_radians(c.latitude)), 0.05);
        c.longitude = std::remainder(c.longitude, 360.0);
    }
    if (in->dragging_right)
    {
        c.heading = std::remainder(c.heading + dx * 0.2, 360.0);
        c.pitch = std::clamp(c.pitch + dy * 0.2, 0.0, 90.0);
    }
}

void on_scroll(GLFWwindow* window, double /*dx*/, double dy)
{
    auto* in = static_cast<input*>(glfwGetWindowUserPointer(window));
    sample::camera& c = in->s->camera;
    c.altitude = std::clamp(c.altitude * std::pow(0.85, dy), 50.0, 5.0e7);
}

void on_key(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/)
{
    if (action != GLFW_PRESS)
    {
        return;
    }
    auto* in = static_cast<input*>(glfwGetWindowUserPointer(window));
    if (key == GLFW_KEY_ESCAPE)
    {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
    else if (key == GLFW_KEY_L)
    {
        in->s->sunlight = !in->s->sunlight;
    }
    else if (key >= GLFW_KEY_1 && key < GLFW_KEY_1 + static_cast<int>(places.size()))
    {
        in->s->camera = places[static_cast<std::size_t>(key - GLFW_KEY_1)].camera;
    }
}

int run_interactive(const options& o, GLFWwindow* window, const host_gl& gl, vgl::context& ctx)
{
    scene s;
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

    input in;
    in.s = &s;
    glfwSetWindowUserPointer(window, &in);
    glfwSetMouseButtonCallback(window, on_mouse_button);
    glfwSetCursorPosCallback(window, on_cursor);
    glfwSetScrollCallback(window, on_scroll);
    glfwSetKeyCallback(window, on_key);

    target t;
    auto last_title = std::chrono::steady_clock::now();
    int frames = 0;
    while (glfwWindowShouldClose(window) == 0)
    {
        glfwPollEvents();
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        if (width == 0 || height == 0)
        {
            glfwWaitEvents(); // Minimized.
            continue;
        }
        if (width != t.width || height != t.height)
        {
            if (t.framebuffer != 0)
            {
                target_destroy(gl, t);
            }
            target_create(gl, t, width, height, s.depth == VGL_DEPTH_CONVENTION_REVERSED_Z);
        }

        // Stay above the terrain.
        const vgl::expected<vgl::height_sample> ground =
            ctx.height_query(vgl::degrees_to_radians(s.camera.latitude),
                             vgl::degrees_to_radians(s.camera.longitude));
        if (ground && ground->status == vgl::result::success)
        {
            s.camera.altitude = std::max(s.camera.altitude, ground->height + 50.0);
        }

        view_set(ctx, s, width, height);
        static_cast<void>(ctx.update());
        clear(gl, s, t);
        static_cast<void>(draw(*backend, s));

        // Copy the frame to the window.
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, t.framebuffer);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        gl.BlitFramebuffer(
            0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glfwSwapBuffers(window);

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
    }
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
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    options o;
    if (!parse(argc, argv, o))
    {
        std::fprintf(stderr,
                     "usage: vgl-gl [--data <directory>] [--es] [--egl] "
                     "[--depth logarithmic|reversed|conventional] [--check] "
                     "[--save <directory>]\n");
        return 2;
    }

    glfwSetErrorCallback(
        [](int, const char* message) { std::fprintf(stderr, "glfw: %s\n", message); });
    if (glfwInit() == 0)
    {
        return 1;
    }
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
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    const host_gl gl = load_host_gl();
    std::printf("%s\n%s\n\n",
                reinterpret_cast<const char*>(gl.GetString(GL_VERSION)),
                reinterpret_cast<const char*>(gl.GetString(GL_RENDERER)));

    // The defaults: a thread pool for decoding and reading, and a file reader on it.
    vgl::expected<vgl::thread_pool> pool = vgl::thread_pool::create();
    if (!pool)
    {
        std::fprintf(stderr, "could not create the thread pool\n");
        return 1;
    }
    vgl::expected<vgl::file_reader> reader = vgl::file_reader::create(*pool);
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
    const std::string terrain_uri = o.data + "/packs/gebco";
    const std::string geoid_uri = o.data + "/packs/egm2008";
    vgl::expected<vgl::pack> terrain = vgl::pack::create(*ctx, terrain_uri.c_str());
    vgl::expected<vgl::pack> geoid = vgl::pack::create(*ctx, geoid_uri.c_str());
    if (!terrain || !geoid)
    {
        std::fprintf(stderr, "could not add the packs\n");
        return 1;
    }

    const int code = o.check ? run_check(o, gl, *ctx) : run_interactive(o, window, gl, *ctx);

    // Leave in order: the backend went with its run; then packs and context, the reader and
    // the pool, and last the window with its OpenGL context.
    terrain->reset();
    geoid->reset();
    ctx->reset();
    reader->reset();
    pool->reset();
    glfwDestroyWindow(window);
    glfwTerminate();
    return code;
}
