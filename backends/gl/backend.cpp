// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The OpenGL backend (vgl/gl.h): a consumer of the render interface (docs/render-interface.md)
// that turns each frame's resource lists into buffers, vertex arrays and textures, and its draw
// list into one draw call per tile, with up to four imagery layers, and the atmosphere's sky
// after them.

#include "atmosphere_shader.hpp"
#include "backends/common/atmosphere_view.hpp"
#include "backends/common/stars.hpp"
#include "common/host_memory.hpp"
#include "common/pointer_map.hpp"
#include "gl_api.hpp"
#include "shaders.hpp"

#include <vgl/gl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

using namespace vgl::gl;
using vgl::backends::atmosphere_view;
using vgl::common::memory;

namespace
{

enum class resource_kind : std::uint8_t
{
    index_list,
    mesh,
    texture,
};

// Imagery layers a draw blends; a tile with more draws the top ones.
constexpr std::uint32_t max_layers = 4;

// Units 0 to 3 hold the layers, and the next one the mesh's samples.
constexpr std::uint32_t samples_unit = max_layers;
constexpr std::uint32_t texture_units = max_layers + 1;

// The OpenGL objects of one render resource. Its address is the handle the context echoes in
// draw items (vgl_frame_resource_handle_set).
struct gl_resource
{
    resource_kind kind = resource_kind::index_list;
    GLuint buffer = 0;             // Indices or vertices.
    GLuint vertex_array = 0;       // Meshes: the vertex layout and the index list's buffer.
    GLuint texture = 0;            // Textures, and meshes' samples.
    std::uint32_t grid = 0;        // Meshes: vertices along a side of the grid, or 0 if unknown.
    std::uint32_t sample_grid = 0; // Meshes: samples along a side.
    float min_height = 0.0f;       // Meshes: the lowest terrain height; the skirts hang below it.
};

// Where a program takes the atmosphere's values (shaders.hpp).
struct atmosphere_uniforms
{
    GLint enabled = -1;
    GLint to_planet = -1;
    GLint camera_km = -1;
    GLint camera_height = -1;
    GLint sun_planet = -1;
};

// The host's state that drawing changes (NFR-08; vgl/gl.h lists it).
struct saved_state
{
    GLint program = 0;
    GLint vertex_array = 0;
    GLint array_buffer = 0;
    GLint copy_write_buffer = 0;
    GLboolean depth_test = GL_FALSE;
    GLint depth_func = 0;
    GLint depth_mask = 0;
    GLboolean cull_face = GL_FALSE;
    GLint cull_face_mode = 0;
    GLint front_face = 0;
    GLboolean blend = GL_FALSE;
    GLint blend_src_rgb = 0;
    GLint blend_dst_rgb = 0;
    GLint blend_src_alpha = 0;
    GLint blend_dst_alpha = 0;
    GLint blend_equation_rgb = 0;
    GLint blend_equation_alpha = 0;
    GLboolean primitive_restart = GL_FALSE;
    GLboolean primitive_restart_fixed_index = GL_FALSE;
    GLint clip_origin = 0;
    GLint clip_depth_mode = 0;
    GLint active_texture = 0;
    std::array<GLint, texture_units> textures = {};
    std::array<GLint, texture_units> samplers = {};
    GLint unpack_buffer = 0;
    GLint unpack_alignment = 0;
    GLint unpack_row_length = 0;
    GLint unpack_skip_rows = 0;
    GLint unpack_skip_pixels = 0;
};

constexpr std::size_t message_size = 1024;

} // namespace

struct vgl_gl_backend
{
    explicit vgl_gl_backend(const memory& m) noexcept : mem(m), resources(m) {}

    memory mem;
    vgl_context* context = nullptr;
    vgl_gl_get_proc_address_fn get_proc_address = nullptr;
    void* get_proc_address_user_data = nullptr;
    vgl_depth_convention depth = VGL_DEPTH_CONVENTION_LOGARITHMIC;

    // Valid while ready: after a successful create or reset.
    bool ready = false;
    functions gl;
    device dev;
    GLuint program = 0;
    GLuint sampler = 0;
    GLint u_model_view = -1;
    GLint u_projection = -1;
    GLint u_light = -1;
    GLint u_log_depth = -1;
    GLint u_layer_count = -1;
    GLint u_layer_uv = -1;
    GLint u_layer_opacity = -1;
    GLint u_debug = -1;
    GLint u_level = -1;
    GLint u_imagery_coarser = -1;
    GLint u_cells = -1;
    GLint u_min_height = -1;
    GLint u_contour_interval = -1;
    GLint u_geoid = -1;
    GLint u_centre = -1;
    GLint u_sea = -1;
    GLint u_samples = -1;
    GLint u_sample_cells = -1;
    std::array<GLint, max_layers> u_layers = {-1, -1, -1, -1};
    atmosphere_uniforms u_terrain_atmosphere;

    // The stars: the host's, as quads (backends/common/stars.hpp), kept on the CPU to be
    // uploaded again after a reset. The buffer is made when they are first drawn.
    vgl::backends::star_vertex* star_vertices = nullptr;
    std::uint32_t star_count = 0;
    bool stars_uploaded = false;
    GLuint stars_program = 0;
    GLuint stars_buffer = 0;
    GLuint stars_vertex_array = 0;
    atmosphere_uniforms u_stars_atmosphere;
    GLint u_stars_celestial = -1;
    GLint u_stars_projection = -1;
    GLint u_stars_viewport = -1;
    GLint u_stars_far_depth = -1;

    // The sky, drawn as one triangle without vertex buffers, from an empty vertex array.
    GLuint sky_program = 0;
    GLuint sky_vertex_array = 0;
    atmosphere_uniforms u_sky_atmosphere;
    GLint u_sky_inverse_projection = -1;
    GLint u_sky_far_depth = -1;
    GLint u_sky_mid_depth = -1;

    vgl::common::pointer_map<gl_resource> resources;
};

namespace
{

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
// A C-style variadic function, as the core's logger is (src/foundation/log.cpp), to format with
// printf rules. On some platforms va_list is an array type, which also triggers the array decay
// check.
// NOLINTBEGIN(cert-dcl50-cpp,cppcoreguidelines-pro-type-vararg,cppcoreguidelines-pro-bounds-array-to-pointer-decay)
void log(const vgl_gl_backend& b, vgl_log_level level, const char* format, ...) noexcept
{
    std::array<char, message_size> text{};
    va_list args;
    va_start(args, format);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized): args is initialized by va_start.
    const int written = std::vsnprintf(text.data(), text.size(), format, args);
    va_end(args);
    if (written >= 0)
    {
        static_cast<void>(vgl_context_log(b.context, level, text.data()));
    }
}
// NOLINTEND(cert-dcl50-cpp,cppcoreguidelines-pro-type-vararg,cppcoreguidelines-pro-bounds-array-to-pointer-decay)

GLuint compile_shader(const vgl_gl_backend& b, const char* function, GLenum type, const char* body)
{
    const functions& gl = b.gl;
    const std::array<const GLchar*, 4> sources{
        b.dev.es ? es_preamble : desktop_preamble,
        b.depth == VGL_DEPTH_CONVENTION_LOGARITHMIC ? logarithmic_define : "",
        vgl::backends::atmosphere_shader,
        body,
    };
    const GLuint shader = gl.CreateShader(type);
    gl.ShaderSource(shader, static_cast<GLsizei>(sources.size()), sources.data(), nullptr);
    gl.CompileShader(shader);
    GLint status = GL_FALSE;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE)
    {
        std::array<GLchar, message_size> text{};
        gl.GetShaderInfoLog(shader, static_cast<GLsizei>(text.size()), nullptr, text.data());
        log(b,
            VGL_LOG_LEVEL_ERROR,
            "%s: the %s shader failed to compile: %s",
            function,
            type == GL_VERTEX_SHADER ? "vertex" : "fragment",
            text.data());
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

// Compiles and links a program, or returns 0.
GLuint link_program(const vgl_gl_backend& b,
                    const char* function,
                    const char* name,
                    const char* vertex,
                    const char* fragment)
{
    const functions& gl = b.gl;
    const GLuint vs = compile_shader(b, function, GL_VERTEX_SHADER, vertex);
    const GLuint fs = vs == 0 ? 0 : compile_shader(b, function, GL_FRAGMENT_SHADER, fragment);
    if (fs == 0)
    {
        if (vs != 0)
        {
            gl.DeleteShader(vs);
        }
        return 0;
    }
    const GLuint program = gl.CreateProgram();
    gl.AttachShader(program, vs);
    gl.AttachShader(program, fs);
    gl.LinkProgram(program);
    gl.DetachShader(program, vs);
    gl.DetachShader(program, fs);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint status = GL_FALSE;
    gl.GetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE)
    {
        std::array<GLchar, message_size> text{};
        gl.GetProgramInfoLog(program, static_cast<GLsizei>(text.size()), nullptr, text.data());
        log(b,
            VGL_LOG_LEVEL_ERROR,
            "%s: the %s program failed to link: %s",
            function,
            name,
            text.data());
        gl.DeleteProgram(program);
        return 0;
    }
    return program;
}

atmosphere_uniforms atmosphere_locations(const functions& gl, GLuint program)
{
    return {gl.GetUniformLocation(program, "u_atmosphere"),
            gl.GetUniformLocation(program, "u_to_planet"),
            gl.GetUniformLocation(program, "u_camera_km"),
            gl.GetUniformLocation(program, "u_camera_height"),
            gl.GetUniformLocation(program, "u_sun_planet")};
}

vgl_result build_program(vgl_gl_backend& b, const char* function)
{
    const functions& gl = b.gl;
    const GLuint program =
        link_program(b, function, "terrain", terrain_vertex_shader, terrain_fragment_shader);
    if (program == 0)
    {
        return VGL_RESULT_BACKEND_ERROR;
    }
    const GLuint sky = link_program(b, function, "sky", sky_vertex_shader, sky_fragment_shader);
    const GLuint stars =
        sky == 0 ? 0
                 : link_program(b, function, "stars", stars_vertex_shader, stars_fragment_shader);
    if (stars == 0)
    {
        gl.DeleteProgram(program);
        if (sky != 0)
        {
            gl.DeleteProgram(sky);
        }
        return VGL_RESULT_BACKEND_ERROR;
    }
    b.stars_program = stars;
    b.u_stars_atmosphere = atmosphere_locations(gl, stars);
    b.u_stars_celestial = gl.GetUniformLocation(stars, "u_celestial");
    b.u_stars_projection = gl.GetUniformLocation(stars, "u_projection");
    b.u_stars_viewport = gl.GetUniformLocation(stars, "u_viewport");
    b.u_stars_far_depth = gl.GetUniformLocation(stars, "u_far_depth");
    b.program = program;
    b.sky_program = sky;
    b.u_terrain_atmosphere = atmosphere_locations(gl, program);
    b.u_sky_atmosphere = atmosphere_locations(gl, sky);
    b.u_sky_inverse_projection = gl.GetUniformLocation(sky, "u_inverse_projection");
    b.u_sky_far_depth = gl.GetUniformLocation(sky, "u_far_depth");
    b.u_sky_mid_depth = gl.GetUniformLocation(sky, "u_mid_depth");
    gl.GenVertexArrays(1, &b.sky_vertex_array);
    b.u_model_view = gl.GetUniformLocation(program, "u_model_view");
    b.u_projection = gl.GetUniformLocation(program, "u_projection");
    b.u_light = gl.GetUniformLocation(program, "u_light");
    b.u_log_depth = gl.GetUniformLocation(program, "u_log_depth");
    b.u_layer_count = gl.GetUniformLocation(program, "u_layer_count");
    b.u_layer_uv = gl.GetUniformLocation(program, "u_layer_uv");
    b.u_layer_opacity = gl.GetUniformLocation(program, "u_layer_opacity");
    b.u_debug = gl.GetUniformLocation(program, "u_debug");
    b.u_level = gl.GetUniformLocation(program, "u_level");
    b.u_imagery_coarser = gl.GetUniformLocation(program, "u_imagery_coarser");
    b.u_cells = gl.GetUniformLocation(program, "u_cells");
    b.u_min_height = gl.GetUniformLocation(program, "u_min_height");
    b.u_contour_interval = gl.GetUniformLocation(program, "u_contour_interval");
    b.u_geoid = gl.GetUniformLocation(program, "u_geoid");
    b.u_centre = gl.GetUniformLocation(program, "u_centre");
    b.u_sea = gl.GetUniformLocation(program, "u_sea");
    b.u_samples = gl.GetUniformLocation(program, "u_samples");
    b.u_sample_cells = gl.GetUniformLocation(program, "u_sample_cells");
    const std::array<const char*, max_layers> names = {
        "u_layer0", "u_layer1", "u_layer2", "u_layer3"};
    for (std::uint32_t k = 0; k < max_layers; ++k)
    {
        b.u_layers[k] = gl.GetUniformLocation(program, names[k]);
    }

    // Trilinear filtering, anisotropic where the device has it, and edges clamped: tiles carry
    // their neighbours' pixels in their border instead.
    gl.GenSamplers(1, &b.sampler);
    gl.SamplerParameteri(b.sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl.SamplerParameteri(b.sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.SamplerParameteri(b.sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.SamplerParameteri(b.sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (b.dev.max_anisotropy > 1.0f)
    {
        gl.SamplerParameterf(
            b.sampler, GL_TEXTURE_MAX_ANISOTROPY, std::min(8.0f, b.dev.max_anisotropy));
    }
    return VGL_RESULT_SUCCESS;
}

// Loads the functions and builds the program on the current OpenGL context.
vgl_result start(vgl_gl_backend& b, const char* function)
{
    std::array<char, message_size> problem{};
    const vgl_result r = load(b.get_proc_address,
                              b.get_proc_address_user_data,
                              b.depth == VGL_DEPTH_CONVENTION_REVERSED_Z,
                              b.gl,
                              b.dev,
                              problem.data(),
                              problem.size());
    if (r != VGL_RESULT_SUCCESS)
    {
        log(b, VGL_LOG_LEVEL_ERROR, "%s: %s", function, problem.data());
        return r;
    }
    const vgl_result built = build_program(b, function);
    if (built != VGL_RESULT_SUCCESS)
    {
        return built;
    }
    b.ready = true;
    log(b,
        VGL_LOG_LEVEL_INFO,
        "%s: drawing with OpenGL%s %d.%d",
        function,
        b.dev.es ? " ES" : "",
        b.dev.major,
        b.dev.minor);
    return VGL_RESULT_SUCCESS;
}

// Frees the records of every resource, deleting their OpenGL objects if the context still has
// them.
void forget_resources(vgl_gl_backend& b, bool delete_objects)
{
    b.resources.for_each([&](std::uint64_t, gl_resource* r) {
        if (delete_objects)
        {
            if (r->vertex_array != 0)
            {
                b.gl.DeleteVertexArrays(1, &r->vertex_array);
            }
            if (r->texture != 0)
            {
                b.gl.DeleteTextures(1, &r->texture);
            }
            if (r->buffer != 0)
            {
                b.gl.DeleteBuffers(1, &r->buffer);
            }
        }
        b.mem.destroy(r);
    });
    b.resources.clear();
}

// Frees the CPU copy of the stars.
void free_stars(vgl_gl_backend& b)
{
    if (b.star_vertices != nullptr)
    {
        b.mem.deallocate(b.star_vertices,
                         std::size_t{b.star_count} * vgl::backends::star_vertex_count *
                             sizeof(vgl::backends::star_vertex),
                         alignof(vgl::backends::star_vertex));
    }
    b.star_vertices = nullptr;
    b.star_count = 0;
    b.stars_uploaded = false;
}

void stop(vgl_gl_backend& b, bool delete_objects)
{
    forget_resources(b, delete_objects && b.ready);
    if (delete_objects && b.ready)
    {
        b.gl.DeleteProgram(b.program);
        b.gl.DeleteProgram(b.sky_program);
        b.gl.DeleteVertexArrays(1, &b.sky_vertex_array);
        b.gl.DeleteProgram(b.stars_program);
        if (b.stars_vertex_array != 0)
        {
            b.gl.DeleteVertexArrays(1, &b.stars_vertex_array);
            b.gl.DeleteBuffers(1, &b.stars_buffer);
        }
        b.gl.DeleteSamplers(1, &b.sampler);
    }
    b.program = 0;
    b.sky_program = 0;
    b.sky_vertex_array = 0;
    b.stars_program = 0;
    b.stars_buffer = 0;
    b.stars_vertex_array = 0;
    b.stars_uploaded = false;
    b.sampler = 0;
    b.ready = false;
}

saved_state save_state(const vgl_gl_backend& b)
{
    const functions& gl = b.gl;
    saved_state s;
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vertex_array);
    gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.array_buffer);
    gl.GetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &s.copy_write_buffer);
    s.depth_test = gl.IsEnabled(GL_DEPTH_TEST);
    gl.GetIntegerv(GL_DEPTH_FUNC, &s.depth_func);
    gl.GetIntegerv(GL_DEPTH_WRITEMASK, &s.depth_mask);
    s.cull_face = gl.IsEnabled(GL_CULL_FACE);
    gl.GetIntegerv(GL_CULL_FACE_MODE, &s.cull_face_mode);
    gl.GetIntegerv(GL_FRONT_FACE, &s.front_face);
    s.blend = gl.IsEnabled(GL_BLEND);
    gl.GetIntegerv(GL_BLEND_SRC_RGB, &s.blend_src_rgb);
    gl.GetIntegerv(GL_BLEND_DST_RGB, &s.blend_dst_rgb);
    gl.GetIntegerv(GL_BLEND_SRC_ALPHA, &s.blend_src_alpha);
    gl.GetIntegerv(GL_BLEND_DST_ALPHA, &s.blend_dst_alpha);
    gl.GetIntegerv(GL_BLEND_EQUATION_RGB, &s.blend_equation_rgb);
    gl.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &s.blend_equation_alpha);
    if (b.dev.primitive_restart)
    {
        s.primitive_restart = gl.IsEnabled(GL_PRIMITIVE_RESTART);
    }
    if (b.dev.primitive_restart_fixed_index)
    {
        s.primitive_restart_fixed_index = gl.IsEnabled(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    }
    if (b.depth == VGL_DEPTH_CONVENTION_REVERSED_Z)
    {
        gl.GetIntegerv(GL_CLIP_ORIGIN, &s.clip_origin);
        gl.GetIntegerv(GL_CLIP_DEPTH_MODE, &s.clip_depth_mode);
    }
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &s.active_texture);
    for (std::uint32_t k = 0; k < texture_units; ++k)
    {
        gl.ActiveTexture(GL_TEXTURE0 + k);
        gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s.textures[k]);
        gl.GetIntegerv(GL_SAMPLER_BINDING, &s.samplers[k]);
    }
    gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s.unpack_buffer);
    gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &s.unpack_alignment);
    gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &s.unpack_row_length);
    gl.GetIntegerv(GL_UNPACK_SKIP_ROWS, &s.unpack_skip_rows);
    gl.GetIntegerv(GL_UNPACK_SKIP_PIXELS, &s.unpack_skip_pixels);
    return s;
}

void set_enabled(const functions& gl, GLenum cap, bool enabled)
{
    if (enabled)
    {
        gl.Enable(cap);
    }
    else
    {
        gl.Disable(cap);
    }
}

void restore_state(const vgl_gl_backend& b, const saved_state& s)
{
    const functions& gl = b.gl;
    gl.UseProgram(static_cast<GLuint>(s.program));
    gl.BindVertexArray(static_cast<GLuint>(s.vertex_array));
    gl.BindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(s.array_buffer));
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, static_cast<GLuint>(s.copy_write_buffer));
    set_enabled(gl, GL_DEPTH_TEST, s.depth_test != GL_FALSE);
    gl.DepthFunc(static_cast<GLenum>(s.depth_func));
    gl.DepthMask(s.depth_mask != 0 ? GL_TRUE : GL_FALSE);
    set_enabled(gl, GL_CULL_FACE, s.cull_face != GL_FALSE);
    gl.CullFace(static_cast<GLenum>(s.cull_face_mode));
    gl.FrontFace(static_cast<GLenum>(s.front_face));
    set_enabled(gl, GL_BLEND, s.blend != GL_FALSE);
    gl.BlendFuncSeparate(static_cast<GLenum>(s.blend_src_rgb),
                         static_cast<GLenum>(s.blend_dst_rgb),
                         static_cast<GLenum>(s.blend_src_alpha),
                         static_cast<GLenum>(s.blend_dst_alpha));
    gl.BlendEquationSeparate(static_cast<GLenum>(s.blend_equation_rgb),
                             static_cast<GLenum>(s.blend_equation_alpha));
    if (b.dev.primitive_restart)
    {
        set_enabled(gl, GL_PRIMITIVE_RESTART, s.primitive_restart != GL_FALSE);
    }
    if (b.dev.primitive_restart_fixed_index)
    {
        set_enabled(
            gl, GL_PRIMITIVE_RESTART_FIXED_INDEX, s.primitive_restart_fixed_index != GL_FALSE);
    }
    if (b.depth == VGL_DEPTH_CONVENTION_REVERSED_Z)
    {
        gl.ClipControl(static_cast<GLenum>(s.clip_origin), static_cast<GLenum>(s.clip_depth_mode));
    }
    for (std::uint32_t k = 0; k < texture_units; ++k)
    {
        gl.ActiveTexture(GL_TEXTURE0 + k);
        gl.BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(s.textures[k]));
        gl.BindSampler(k, static_cast<GLuint>(s.samplers[k]));
    }
    gl.ActiveTexture(static_cast<GLenum>(s.active_texture));
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(s.unpack_buffer));
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, s.unpack_alignment);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, s.unpack_row_length);
    gl.PixelStorei(GL_UNPACK_SKIP_ROWS, s.unpack_skip_rows);
    gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, s.unpack_skip_pixels);
}

const void* buffer_offset(std::uintptr_t offset)
{
    // OpenGL takes buffer offsets as pointers.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<const void*>(offset);
}

void create_index_list(vgl_gl_backend& b, vgl_frame* frame, const vgl_index_list_data& d)
{
    const functions& gl = b.gl;
    auto* r = b.mem.create<gl_resource>();
    if (r == nullptr || !b.resources.insert(d.resource, r))
    {
        b.mem.destroy(r);
        static_cast<void>(vgl_frame_resource_failed(frame, d.resource, VGL_RESULT_OUT_OF_MEMORY));
        return;
    }
    r->kind = resource_kind::index_list;
    gl.GenBuffers(1, &r->buffer);
    // GL_COPY_WRITE_BUFFER rather than GL_ELEMENT_ARRAY_BUFFER, whose binding belongs to the
    // vertex array that is bound: the host's.
    gl.BindBuffer(GL_COPY_WRITE_BUFFER, r->buffer);
    gl.BufferData(GL_COPY_WRITE_BUFFER,
                  static_cast<GLsizeiptr>(std::size_t{d.index_count} * d.index_size),
                  d.indices,
                  GL_STATIC_DRAW);
}

void create_mesh(vgl_gl_backend& b, vgl_frame* frame, const vgl_mesh_data& d)
{
    const functions& gl = b.gl;
    const gl_resource* indices = b.resources.find(d.index_list);
    if (indices == nullptr || indices->kind != resource_kind::index_list)
    {
        // Its index list failed (docs/render-interface.md, section 4.2).
        static_cast<void>(vgl_frame_resource_failed(frame, d.resource, VGL_RESULT_NOT_FOUND));
        return;
    }
    if (d.samples == nullptr || d.sample_grid < 2 || d.sample_size != VGL_TERRAIN_SAMPLE_SIZE)
    {
        static_cast<void>(vgl_frame_resource_failed(frame, d.resource, VGL_RESULT_NOT_SUPPORTED));
        return;
    }
    auto* r = b.mem.create<gl_resource>();
    if (r == nullptr || !b.resources.insert(d.resource, r))
    {
        b.mem.destroy(r);
        static_cast<void>(vgl_frame_resource_failed(frame, d.resource, VGL_RESULT_OUT_OF_MEMORY));
        return;
    }
    r->kind = resource_kind::mesh;
    // A grid of g x g vertices and its skirts' 4 (g - 1) (docs/render-interface.md, section 6).
    const auto grid = static_cast<std::uint32_t>(
        std::lround(std::sqrt(8.0 + static_cast<double>(d.vertex_count)) - 2.0));
    if (grid > 1 && std::size_t{grid} * grid + 4 * (std::size_t{grid} - 1) == d.vertex_count)
    {
        r->grid = grid;
    }
    // Vertex heights are floats, so a skirt's is below the lowest terrain height as a float.
    r->min_height = static_cast<float>(d.min_height);
    gl.GenBuffers(1, &r->buffer);
    gl.GenVertexArrays(1, &r->vertex_array);
    gl.BindVertexArray(r->vertex_array);
    gl.BindBuffer(GL_ARRAY_BUFFER, r->buffer);
    gl.BufferData(GL_ARRAY_BUFFER,
                  static_cast<GLsizeiptr>(std::size_t{d.vertex_count} * d.vertex_size),
                  d.vertices,
                  GL_STATIC_DRAW);
    const auto stride = static_cast<GLsizei>(d.vertex_size);
    gl.VertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE, stride, buffer_offset(VGL_TERRAIN_VERTEX_POSITION_OFFSET));
    gl.VertexAttribPointer(
        1, 1, GL_FLOAT, GL_FALSE, stride, buffer_offset(VGL_TERRAIN_VERTEX_HEIGHT_OFFSET));
    gl.VertexAttribPointer(
        2, 2, GL_UNSIGNED_SHORT, GL_TRUE, stride, buffer_offset(VGL_TERRAIN_VERTEX_UV_OFFSET));
    gl.VertexAttribPointer(
        3, 2, GL_SHORT, GL_TRUE, stride, buffer_offset(VGL_TERRAIN_VERTEX_NORMAL_OFFSET));
    for (GLuint i = 0; i < 4; ++i)
    {
        gl.EnableVertexAttribArray(i);
    }
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices->buffer);

    // The samples, as two 32-bit unsigned integers per texel: the height's bits and the
    // normal's two halves, read back in the shader. Integer textures sample the nearest texel
    // only, and the shader interpolates; nothing filters across levels.
    r->sample_grid = d.sample_grid;
    gl.GenTextures(1, &r->texture);
    gl.ActiveTexture(GL_TEXTURE0 + samples_unit);
    gl.BindTexture(GL_TEXTURE_2D, r->texture);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const auto side = static_cast<GLsizei>(d.sample_grid);
    gl.TexImage2D(GL_TEXTURE_2D,
                  0,
                  static_cast<GLint>(GL_RG32UI),
                  side,
                  side,
                  0,
                  GL_RG_INTEGER,
                  GL_UNSIGNED_INT,
                  d.samples);
    static_cast<void>(
        vgl_frame_resource_handle_set(frame, d.resource, reinterpret_cast<std::uintptr_t>(r)));
}

void create_texture(vgl_gl_backend& b, vgl_frame* frame, const vgl_texture_data& d)
{
    const functions& gl = b.gl;
    if (d.format != VGL_TEXTURE_FORMAT_RGBA8 && d.format != VGL_TEXTURE_FORMAT_RGBA8_SRGB)
    {
        static_cast<void>(vgl_frame_resource_failed(frame, d.resource, VGL_RESULT_NOT_SUPPORTED));
        return;
    }
    auto* r = b.mem.create<gl_resource>();
    if (r == nullptr || !b.resources.insert(d.resource, r))
    {
        b.mem.destroy(r);
        static_cast<void>(vgl_frame_resource_failed(frame, d.resource, VGL_RESULT_OUT_OF_MEMORY));
        return;
    }
    r->kind = resource_kind::texture;
    gl.GenTextures(1, &r->texture);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, r->texture);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(d.mip_count) - 1);
    const auto internal_format =
        static_cast<GLint>(d.format == VGL_TEXTURE_FORMAT_RGBA8_SRGB ? GL_SRGB8_ALPHA8 : GL_RGBA8);
    auto w = static_cast<GLsizei>(d.width);
    auto h = static_cast<GLsizei>(d.height);
    for (std::uint32_t level = 0; level < d.mip_count; ++level)
    {
        gl.TexImage2D(GL_TEXTURE_2D,
                      static_cast<GLint>(level),
                      internal_format,
                      w,
                      h,
                      0,
                      GL_RGBA,
                      GL_UNSIGNED_BYTE,
                      d.mip_data[level]);
        w = std::max<GLsizei>(1, w / 2);
        h = std::max<GLsizei>(1, h / 2);
    }
    static_cast<void>(
        vgl_frame_resource_handle_set(frame, d.resource, reinterpret_cast<std::uintptr_t>(r)));
}

void destroy_resource(vgl_gl_backend& b, std::uint64_t id)
{
    gl_resource* r = b.resources.find(id);
    if (r == nullptr)
    {
        return; // It failed to create.
    }
    if (r->vertex_array != 0)
    {
        b.gl.DeleteVertexArrays(1, &r->vertex_array);
    }
    if (r->texture != 0)
    {
        b.gl.DeleteTextures(1, &r->texture);
    }
    if (r->buffer != 0)
    {
        b.gl.DeleteBuffers(1, &r->buffer);
    }
    b.resources.erase(id);
    b.mem.destroy(r);
}

// Applies the frame's resource lists in their order (docs/render-interface.md, section 4.2).
void apply_resources(vgl_gl_backend& b, vgl_frame* frame)
{
    const std::uint64_t* destroys = nullptr;
    std::uint32_t count = 0;
    static_cast<void>(vgl_frame_destroys_get(frame, &destroys, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        destroy_resource(b, destroys[i]);
    }

    const vgl_index_list_data* index_lists = nullptr;
    static_cast<void>(vgl_frame_index_lists_get(frame, &index_lists, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        create_index_list(b, frame, index_lists[i]);
    }

    // Rows are tightly packed in client memory, which the host's unpack state may not say.
    b.gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    b.gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    b.gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    b.gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    b.gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);

    const vgl_mesh_data* meshes = nullptr;
    static_cast<void>(vgl_frame_meshes_get(frame, &meshes, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        create_mesh(b, frame, meshes[i]);
    }

    const vgl_texture_data* textures = nullptr;
    static_cast<void>(vgl_frame_textures_get(frame, &textures, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        create_texture(b, frame, textures[i]);
    }
}

std::array<GLfloat, 16> to_float(const vgl_dmat4& m)
{
    std::array<GLfloat, 16> f{};
    for (std::size_t i = 0; i < f.size(); ++i)
    {
        f[i] = static_cast<GLfloat>(m.m[i]);
    }
    return f;
}

// The record a handle names: its address, as create_mesh and create_texture set it.
const gl_resource* from_handle(std::uint64_t handle)
{
    const auto address = static_cast<std::uintptr_t>(handle);
    return reinterpret_cast<const gl_resource*>(address); // NOLINT(performance-no-int-to-ptr)
}

void set_atmosphere(const functions& gl,
                    const atmosphere_uniforms& u,
                    bool enabled,
                    const atmosphere_view& a)
{
    gl.Uniform1i(u.enabled, enabled ? 1 : 0);
    if (enabled)
    {
        gl.UniformMatrix3fv(u.to_planet, 1, GL_FALSE, a.to_planet.data());
        gl.Uniform3f(u.camera_km, a.camera_km[0], a.camera_km[1], a.camera_km[2]);
        gl.Uniform1f(u.camera_height, a.camera_height);
        gl.Uniform3f(u.sun_planet, a.sun_planet[0], a.sun_planet[1], a.sun_planet[2]);
    }
}

// Uploads the stars into a buffer and vertex array, once after each set or reset.
void upload_stars(vgl_gl_backend& b)
{
    const functions& gl = b.gl;
    if (b.stars_vertex_array == 0)
    {
        gl.GenBuffers(1, &b.stars_buffer);
        gl.GenVertexArrays(1, &b.stars_vertex_array);
        gl.BindVertexArray(b.stars_vertex_array);
        gl.BindBuffer(GL_ARRAY_BUFFER, b.stars_buffer);
        constexpr auto stride = static_cast<GLsizei>(sizeof(vgl::backends::star_vertex));
        gl.VertexAttribPointer(
            0, 3, GL_FLOAT, GL_FALSE, stride, buffer_offset(vgl::backends::star_direction_offset));
        gl.VertexAttribPointer(
            1, 2, GL_FLOAT, GL_FALSE, stride, buffer_offset(vgl::backends::star_corner_offset));
        gl.VertexAttribPointer(
            2, 2, GL_FLOAT, GL_FALSE, stride, buffer_offset(vgl::backends::star_light_offset));
        gl.VertexAttribPointer(
            3, 3, GL_FLOAT, GL_FALSE, stride, buffer_offset(vgl::backends::star_color_offset));
        for (GLuint i = 0; i < 4; ++i)
        {
            gl.EnableVertexAttribArray(i);
        }
    }
    gl.BindBuffer(GL_ARRAY_BUFFER, b.stars_buffer);
    gl.BufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(std::size_t{b.star_count} * vgl::backends::star_vertex_count *
                                sizeof(vgl::backends::star_vertex)),
        b.star_vertices,
        GL_STATIC_DRAW);
    b.stars_uploaded = true;
}

// The stars, after the terrain and before the sky: quads at the far depth, where nothing was
// drawn, added to what the framebuffer holds.
void draw_stars(vgl_gl_backend& b,
                const vgl_gl_draw_info& info,
                const vgl_dmat4& projection,
                const vgl_dmat4& view,
                bool atmosphere,
                const atmosphere_view& a)
{
    if (b.star_count == 0)
    {
        return;
    }
    const functions& gl = b.gl;
    if (!b.stars_uploaded)
    {
        upload_stars(b);
    }
    std::array<GLint, 4> viewport{};
    gl.GetIntegerv(GL_VIEWPORT, viewport.data());
    gl.UseProgram(b.stars_program);
    set_atmosphere(gl, b.u_stars_atmosphere, atmosphere, a);
    gl.UniformMatrix3fv(b.u_stars_celestial,
                        1,
                        GL_FALSE,
                        vgl::backends::celestial_to_camera(view, info.sidereal_angle).data());
    gl.UniformMatrix4fv(b.u_stars_projection, 1, GL_FALSE, to_float(projection).data());
    gl.Uniform2f(b.u_stars_viewport,
                 static_cast<GLfloat>(std::max(viewport[2], 1)),
                 static_cast<GLfloat>(std::max(viewport[3], 1)));
    const bool reversed = b.depth == VGL_DEPTH_CONVENTION_REVERSED_Z;
    gl.Uniform1f(b.u_stars_far_depth, reversed ? 0.0f : 1.0f);
    gl.DepthFunc(reversed ? GL_GEQUAL : GL_LEQUAL);
    gl.DepthMask(GL_FALSE);
    gl.Disable(GL_CULL_FACE);
    gl.Enable(GL_BLEND);
    gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
    gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    gl.BindVertexArray(b.stars_vertex_array);
    gl.DrawArrays(
        GL_TRIANGLES, 0, static_cast<GLsizei>(b.star_count * vgl::backends::star_vertex_count));
}

// The sky, after the terrain: one triangle at the far depth, where nothing was drawn, blended
// over what the framebuffer holds by the atmosphere's transmittance.
void draw_sky(const vgl_gl_backend& b, const vgl_dmat4& projection, const atmosphere_view& a)
{
    vgl_dmat4 inverse{};
    if (!vgl::backends::invert(projection, inverse))
    {
        return;
    }
    const functions& gl = b.gl;
    gl.UseProgram(b.sky_program);
    set_atmosphere(gl, b.u_sky_atmosphere, true, a);
    gl.UniformMatrix4fv(b.u_sky_inverse_projection, 1, GL_FALSE, to_float(inverse).data());
    // The far plane is at depth 0 for reversed-Z, with clip control's [0, 1] depth, and at 1
    // otherwise, in OpenGL's [-1, 1]; the logarithmic convention keeps the far plane at 1.
    const bool reversed = b.depth == VGL_DEPTH_CONVENTION_REVERSED_Z;
    gl.Uniform1f(b.u_sky_far_depth, reversed ? 0.0f : 1.0f);
    gl.Uniform1f(b.u_sky_mid_depth, 0.5f);
    gl.DepthFunc(reversed ? GL_GEQUAL : GL_LEQUAL);
    gl.DepthMask(GL_FALSE);
    gl.Disable(GL_CULL_FACE);
    gl.Enable(GL_BLEND);
    gl.BlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ZERO, GL_ONE);
    gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    gl.BindVertexArray(b.sky_vertex_array);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

void draw_items(vgl_gl_backend& b,
                const vgl_frame* frame,
                const vgl_gl_draw_info& info,
                const vgl_dmat4& projection,
                const vgl_dmat4* view)
{
    const functions& gl = b.gl;
    gl.UseProgram(b.program);
    gl.UniformMatrix4fv(b.u_projection, 1, GL_FALSE, to_float(projection).data());
    gl.Uniform1i(b.u_debug, static_cast<GLint>(info.debug_flags));
    gl.Uniform1i(b.u_sea, info.sea_surface != VGL_FALSE ? 1 : 0);
    gl.Uniform1f(b.u_contour_interval,
                 static_cast<GLfloat>(info.contour_interval > 0.0 ? info.contour_interval : 500.0));

    // The atmosphere needs the camera and a sun: without one, it is above the camera.
    const bool atmosphere = info.atmosphere != VGL_FALSE && view != nullptr;
    atmosphere_view air;
    vgl_dvec3 sun = info.sun_direction;
    if (atmosphere)
    {
        air = vgl::backends::atmosphere_for(*view, sun);
        sun = vgl::backends::atmosphere_sun(*view, sun);
    }
    set_atmosphere(gl, b.u_terrain_atmosphere, atmosphere, air);

    // The sun in camera space: only the view's rotation applies to a direction.
    std::array<double, 3> light{};
    if (view != nullptr && (sun.x != 0.0 || sun.y != 0.0 || sun.z != 0.0))
    {
        const double* m = view->m;
        light = {m[0] * sun.x + m[4] * sun.y + m[8] * sun.z,
                 m[1] * sun.x + m[5] * sun.y + m[9] * sun.z,
                 m[2] * sun.x + m[6] * sun.y + m[10] * sun.z};
        const double length =
            std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
        for (double& c : light)
        {
            c /= length;
        }
    }
    gl.Uniform3f(b.u_light,
                 static_cast<GLfloat>(light[0]),
                 static_cast<GLfloat>(light[1]),
                 static_cast<GLfloat>(light[2]));

    if (b.depth == VGL_DEPTH_CONVENTION_LOGARITHMIC)
    {
        const double far =
            info.logarithmic_far > 0.0 ? info.logarithmic_far : VGL_LOGARITHMIC_DEPTH_FAR_DEFAULT;
        gl.Uniform1f(b.u_log_depth, static_cast<GLfloat>(2.0 / std::log2(far + 1.0)));
    }

    gl.Enable(GL_DEPTH_TEST);
    gl.DepthMask(GL_TRUE);
    gl.DepthFunc(b.depth == VGL_DEPTH_CONVENTION_REVERSED_Z ? GL_GREATER : GL_LESS);
    gl.Enable(GL_CULL_FACE);
    gl.CullFace(GL_BACK);
    // Counter-clockwise from outside the ellipsoid (docs/render-interface.md, section 6).
    gl.FrontFace(info.mirrored != VGL_FALSE ? GL_CW : GL_CCW);
    gl.Disable(GL_BLEND);
    // 16-bit index lists may use index 0xFFFF as a vertex.
    if (b.dev.primitive_restart)
    {
        gl.Disable(GL_PRIMITIVE_RESTART);
    }
    if (b.dev.primitive_restart_fixed_index)
    {
        gl.Disable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    }
    if (b.depth == VGL_DEPTH_CONVENTION_REVERSED_Z)
    {
        // Keep the host's origin: some hosts put it at the top left, as Direct3D does.
        GLint origin = GL_LOWER_LEFT;
        gl.GetIntegerv(GL_CLIP_ORIGIN, &origin);
        gl.ClipControl(static_cast<GLenum>(origin), GL_ZERO_TO_ONE);
    }

    // Layer k samples texture unit k.
    for (std::uint32_t k = 0; k < max_layers; ++k)
    {
        gl.Uniform1i(b.u_layers[k], static_cast<GLint>(k));
        gl.BindSampler(k, b.sampler);
    }
    // The samples' texture has its own nearest filtering.
    gl.Uniform1i(b.u_samples, static_cast<GLint>(samples_unit));
    gl.BindSampler(samples_unit, 0);
    std::array<GLuint, texture_units> bound = {};
    for (std::uint32_t k = 0; k < texture_units; ++k)
    {
        gl.ActiveTexture(GL_TEXTURE0 + k);
        gl.BindTexture(GL_TEXTURE_2D, 0);
    }

    const vgl_draw_layer* layers = nullptr;
    std::uint32_t layer_total = 0;
    static_cast<void>(vgl_frame_draw_layers_get(frame, &layers, &layer_total));
    const vgl_draw_item* items = nullptr;
    std::uint32_t count = 0;
    static_cast<void>(vgl_frame_draw_list_get(frame, &items, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const vgl_draw_item& item = items[i];
        // The top layers, if there are more than a draw can blend.
        const std::uint32_t used = std::min(item.layer_count, max_layers);
        const std::uint32_t first = item.first_layer + item.layer_count - used;
        std::array<GLfloat, std::size_t{4} * max_layers> uv = {};
        std::array<GLfloat, max_layers> opacity = {};
        std::uint32_t drawn_layers = 0;
        for (std::uint32_t k = 0; k < used && first + k < layer_total; ++k)
        {
            const vgl_draw_layer& layer = layers[first + k];
            const gl_resource* texture = layer.texture_handle != 0
                                             ? from_handle(layer.texture_handle)
                                             : b.resources.find(layer.texture);
            if (texture == nullptr)
            {
                continue;
            }
            const std::uint32_t unit = drawn_layers++;
            if (bound[unit] != texture->texture)
            {
                gl.ActiveTexture(GL_TEXTURE0 + unit);
                gl.BindTexture(GL_TEXTURE_2D, texture->texture);
                bound[unit] = texture->texture;
            }
            std::copy(std::begin(layer.uv_scale_offset),
                      std::end(layer.uv_scale_offset),
                      uv.begin() + static_cast<std::ptrdiff_t>(std::size_t{4} * unit));
            opacity[unit] = layer.opacity;
        }
        gl.Uniform1i(b.u_layer_count, static_cast<GLint>(drawn_layers));
        gl.Uniform4fv(b.u_geoid, 1, item.geoid_heights);
        gl.Uniform3f(b.u_centre,
                     static_cast<GLfloat>(item.centre.x),
                     static_cast<GLfloat>(item.centre.y),
                     static_cast<GLfloat>(item.centre.z));
        if (info.debug_flags != 0)
        {
            gl.Uniform1f(b.u_level, static_cast<GLfloat>(item.tile.level));
            // The top layer's scale is a little below 1 for the tile's own imagery (its border
            // aside), and halves with each level of an ancestor.
            const float scale = drawn_layers != 0 ? uv[4 * std::size_t{drawn_layers - 1}] : 1.0f;
            const double coarser = std::round(-std::log2(std::max(scale, 1e-6f)));
            gl.Uniform1f(b.u_imagery_coarser, static_cast<GLfloat>(std::max(coarser, 0.0)));
        }
        if (drawn_layers != 0)
        {
            gl.Uniform4fv(b.u_layer_uv, static_cast<GLsizei>(drawn_layers), uv.data());
            gl.Uniform4fv(b.u_layer_opacity, 1, opacity.data());
        }
        const gl_resource* mesh =
            item.mesh_handle != 0 ? from_handle(item.mesh_handle) : b.resources.find(item.mesh);
        if (mesh == nullptr)
        {
            continue;
        }
        gl.UniformMatrix4fv(b.u_model_view, 1, GL_FALSE, item.model_view);
        if (bound[samples_unit] != mesh->texture)
        {
            gl.ActiveTexture(GL_TEXTURE0 + samples_unit);
            gl.BindTexture(GL_TEXTURE_2D, mesh->texture);
            bound[samples_unit] = mesh->texture;
        }
        gl.Uniform1f(b.u_sample_cells, static_cast<GLfloat>(mesh->sample_grid - 1));
        if (info.sea_surface != VGL_FALSE)
        {
            gl.Uniform1f(b.u_min_height, mesh->min_height);
        }
        if ((info.debug_flags & VGL_GL_DEBUG_FLAG_WIREFRAME) != 0)
        {
            gl.Uniform1f(b.u_cells, static_cast<GLfloat>(mesh->grid > 1 ? mesh->grid - 1 : 0));
        }
        gl.BindVertexArray(mesh->vertex_array);
        gl.DrawElements(GL_TRIANGLES,
                        static_cast<GLsizei>(item.index_count),
                        item.index_size == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                        nullptr);
    }
    if (info.stars != VGL_FALSE && view != nullptr)
    {
        draw_stars(b, info, projection, *view, atmosphere, air);
    }
    if (atmosphere)
    {
        draw_sky(b, projection, air);
    }
}

bool finite(const vgl_dmat4* m)
{
    return m == nullptr || std::ranges::all_of(m->m, [](double v) { return std::isfinite(v); });
}

const char* create_info_problem(vgl_result r, const vgl_gl_backend_create_info& info)
{
    if (r == VGL_RESULT_NOT_SUPPORTED)
    {
        return "info has fields of a newer vgl; update the library";
    }
    if (r != VGL_RESULT_SUCCESS)
    {
        return "info->struct_size is too small";
    }
    if (info.get_proc_address == nullptr)
    {
        return "info->get_proc_address is null; pass the host's OpenGL loader";
    }
    return "info->depth_convention is unknown";
}

} // namespace

extern "C" VGL_GL_API vgl_result vgl_gl_backend_create(vgl_context* context,
                                                       const vgl_gl_backend_create_info* info_in,
                                                       vgl_gl_backend** out_backend)
{
    if (out_backend == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    *out_backend = nullptr;
    if (context == nullptr || info_in == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_gl_backend_create_info info{};
    vgl_result r = vgl::common::read_versioned(info_in, sizeof(vgl_gl_backend_create_info), info);
    if (r != VGL_RESULT_SUCCESS || info.get_proc_address == nullptr ||
        info.depth_convention > VGL_DEPTH_CONVENTION_CONVENTIONAL)
    {
        std::array<char, message_size> text{};
        static_cast<void>(std::snprintf(
            text.data(), text.size(), "vgl_gl_backend_create: %s", create_info_problem(r, info)));
        static_cast<void>(vgl_context_log(context, VGL_LOG_LEVEL_ERROR, text.data()));
        return r == VGL_RESULT_NOT_SUPPORTED ? r : VGL_RESULT_INVALID_ARGUMENT;
    }

    vgl_allocator_interface allocator{};
    static_cast<void>(vgl_context_allocator_get(context, &allocator));
    const memory mem(allocator);
    auto* b = mem.create<vgl_gl_backend>(mem);
    if (b == nullptr)
    {
        return VGL_RESULT_OUT_OF_MEMORY;
    }
    b->context = context;
    b->get_proc_address = info.get_proc_address;
    b->get_proc_address_user_data = info.get_proc_address_user_data;
    b->depth = info.depth_convention;

    r = start(*b, "vgl_gl_backend_create");
    if (r != VGL_RESULT_SUCCESS)
    {
        mem.destroy(b);
        return r;
    }
    const vgl_texture_format format = VGL_TEXTURE_FORMAT_RGBA8;
    static_cast<void>(vgl_context_texture_formats_set(context, &format, 1));
    *out_backend = b;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_GL_API vgl_result vgl_gl_backend_destroy(vgl_gl_backend* backend)
{
    if (backend == nullptr)
    {
        return VGL_RESULT_SUCCESS;
    }
    stop(*backend, true);
    free_stars(*backend);
    // A later consumer of the context must receive every resource again.
    static_cast<void>(vgl_context_resources_reset(backend->context));
    const memory mem = backend->mem;
    mem.destroy(backend);
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_GL_API vgl_result vgl_gl_backend_stars_set(vgl_gl_backend* backend,
                                                          const vgl_star* stars,
                                                          std::uint32_t star_count)
{
    if (backend == nullptr || (stars == nullptr && star_count != 0) ||
        star_count > vgl::backends::max_stars)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_gl_backend& b = *backend;
    for (std::uint32_t i = 0; i < star_count; ++i)
    {
        if (!vgl::backends::star_is_valid(stars[i]))
        {
            log(b,
                VGL_LOG_LEVEL_ERROR,
                "vgl_gl_backend_stars_set: star %u is invalid: check that its direction is of "
                "unit length, that its values are finite, its colour from 0 to 1, and reserved 0",
                static_cast<unsigned>(i));
            return VGL_RESULT_INVALID_ARGUMENT;
        }
    }
    const std::size_t count = std::size_t{star_count} * vgl::backends::star_vertex_count;
    vgl::backends::star_vertex* vertices = nullptr;
    if (count != 0)
    {
        vertices = static_cast<vgl::backends::star_vertex*>(b.mem.allocate(
            count * sizeof(vgl::backends::star_vertex), alignof(vgl::backends::star_vertex)));
        if (vertices == nullptr)
        {
            return VGL_RESULT_OUT_OF_MEMORY;
        }
        for (std::uint32_t i = 0; i < star_count; ++i)
        {
            vgl::backends::star_quad(stars[i],
                                     vertices + std::size_t{i} * vgl::backends::star_vertex_count);
        }
    }
    free_stars(b);
    b.star_vertices = vertices;
    b.star_count = star_count;
    b.stars_uploaded = false;
    return VGL_RESULT_SUCCESS;
}

extern "C" VGL_GL_API vgl_result vgl_gl_backend_reset(vgl_gl_backend* backend)
{
    if (backend == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    // The objects went with the old OpenGL context, so they are not deleted.
    stop(*backend, false);
    static_cast<void>(vgl_context_resources_reset(backend->context));
    return start(*backend, "vgl_gl_backend_reset");
}

extern "C" VGL_GL_API vgl_result vgl_gl_backend_draw(vgl_gl_backend* backend,
                                                     const vgl_gl_draw_info* info_in)
{
    if (backend == nullptr)
    {
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    vgl_gl_backend& b = *backend;
    vgl_gl_draw_info info{};
    vgl_result r = vgl::common::read_versioned(info_in, sizeof(vgl_gl_draw_info), info);
    if (r != VGL_RESULT_SUCCESS)
    {
        log(b,
            VGL_LOG_LEVEL_ERROR,
            "vgl_gl_backend_draw: %s",
            r == VGL_RESULT_NOT_SUPPORTED ? "info has fields of a newer vgl; update the library"
                                          : "info->struct_size is too small");
        return r == VGL_RESULT_NOT_SUPPORTED ? r : VGL_RESULT_INVALID_ARGUMENT;
    }
    const vgl_dvec3 sun = info.sun_direction;
    if (!finite(info.view_matrix) || !finite(info.projection_matrix) || !std::isfinite(sun.x) ||
        !std::isfinite(sun.y) || !std::isfinite(sun.z) || !std::isfinite(info.logarithmic_far) ||
        info.logarithmic_far < 0.0)
    {
        log(b,
            VGL_LOG_LEVEL_ERROR,
            "vgl_gl_backend_draw: info has a matrix or sun direction that is not finite, or a "
            "negative far distance");
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    constexpr std::uint32_t known_flags =
        VGL_GL_DEBUG_FLAG_WIREFRAME | VGL_GL_DEBUG_FLAG_LEVEL_COLORS |
        VGL_GL_DEBUG_FLAG_IMAGERY_SOURCE | VGL_GL_DEBUG_FLAG_HEIGHT_COLORS |
        VGL_GL_DEBUG_FLAG_CONTOURS;
    if ((info.debug_flags & ~known_flags) != 0 || info.reserved != 0 || info.reserved2 != 0 ||
        info.reserved3 != 0 || info.reserved4 != 0 || !std::isfinite(info.sidereal_angle) ||
        !std::isfinite(info.contour_interval) || info.contour_interval < 0.0)
    {
        log(b,
            VGL_LOG_LEVEL_ERROR,
            "vgl_gl_backend_draw: info->debug_flags has unknown flags, info->reserved is not "
            "zero, or info->contour_interval is negative or not finite");
        return VGL_RESULT_INVALID_ARGUMENT;
    }
    if (!b.ready)
    {
        log(b,
            VGL_LOG_LEVEL_ERROR,
            "vgl_gl_backend_draw: the backend has no OpenGL context since a failed reset; call "
            "vgl_gl_backend_reset again");
        return VGL_RESULT_INVALID_OPERATION;
    }

    vgl_view view{};
    view.struct_size = sizeof(view);
    const bool has_view = vgl_context_view_get(b.context, &view) == VGL_RESULT_SUCCESS;
    // The host's matrices, or else the context's view, or else none: nothing is selected then.
    const vgl_dmat4* projection = has_view ? &view.projection_matrix : nullptr;
    const vgl_dmat4* view_matrix = has_view ? &view.view_matrix : nullptr;
    if (info.projection_matrix != nullptr)
    {
        projection = info.projection_matrix;
    }
    if (info.view_matrix != nullptr)
    {
        view_matrix = info.view_matrix;
    }

    vgl_frame_begin_info frame_info{};
    frame_info.struct_size = sizeof(frame_info);
    frame_info.upload_budget = info.upload_budget;
    frame_info.view_matrix = info.view_matrix;
    vgl_frame* frame = nullptr;
    r = vgl_context_frame_begin(b.context, &frame_info, &frame);
    if (r != VGL_RESULT_SUCCESS)
    {
        return r;
    }
    const saved_state saved = save_state(b);
    apply_resources(b, frame);
    if (projection != nullptr)
    {
        draw_items(b, frame, info, *projection, view_matrix);
    }
    restore_state(b, saved);
    return vgl_frame_end(frame);
}
