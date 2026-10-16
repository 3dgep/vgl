// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The OpenGL backend (vgl/gl.h): a consumer of the render interface (docs/render-interface.md)
// that turns each frame's resource lists into buffers and vertex arrays, and its draw list into
// one draw call per tile.

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
using vgl::common::memory;

namespace
{

enum class resource_kind : std::uint8_t
{
    index_list,
    mesh,
};

// The OpenGL objects of one render resource. Its address is the handle the context echoes in
// draw items (vgl_frame_resource_handle_set).
struct gl_resource
{
    resource_kind kind = resource_kind::index_list;
    GLuint buffer = 0;       // Indices or vertices.
    GLuint vertex_array = 0; // Meshes: the vertex layout and the index list's buffer.
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
    GLboolean primitive_restart = GL_FALSE;
    GLboolean primitive_restart_fixed_index = GL_FALSE;
    GLint clip_origin = 0;
    GLint clip_depth_mode = 0;
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
    GLint u_model_view = -1;
    GLint u_projection = -1;
    GLint u_light = -1;
    GLint u_log_depth = -1;

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
    const std::array<const GLchar*, 3> sources{
        b.dev.es ? es_preamble : desktop_preamble,
        b.depth == VGL_DEPTH_CONVENTION_LOGARITHMIC ? logarithmic_define : "",
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

vgl_result build_program(vgl_gl_backend& b, const char* function)
{
    const functions& gl = b.gl;
    const GLuint vs = compile_shader(b, function, GL_VERTEX_SHADER, terrain_vertex_shader);
    const GLuint fs =
        vs == 0 ? 0 : compile_shader(b, function, GL_FRAGMENT_SHADER, terrain_fragment_shader);
    if (fs == 0)
    {
        if (vs != 0)
        {
            gl.DeleteShader(vs);
        }
        return VGL_RESULT_BACKEND_ERROR;
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
            "%s: the terrain program failed to link: %s",
            function,
            text.data());
        gl.DeleteProgram(program);
        return VGL_RESULT_BACKEND_ERROR;
    }
    b.program = program;
    b.u_model_view = gl.GetUniformLocation(program, "u_model_view");
    b.u_projection = gl.GetUniformLocation(program, "u_projection");
    b.u_light = gl.GetUniformLocation(program, "u_light");
    b.u_log_depth = gl.GetUniformLocation(program, "u_log_depth");
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
            b.gl.DeleteBuffers(1, &r->buffer);
        }
        b.mem.destroy(r);
    });
    b.resources.clear();
}

void stop(vgl_gl_backend& b, bool delete_objects)
{
    forget_resources(b, delete_objects && b.ready);
    if (delete_objects && b.ready)
    {
        b.gl.DeleteProgram(b.program);
    }
    b.program = 0;
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
    auto* r = b.mem.create<gl_resource>();
    if (r == nullptr || !b.resources.insert(d.resource, r))
    {
        b.mem.destroy(r);
        static_cast<void>(vgl_frame_resource_failed(frame, d.resource, VGL_RESULT_OUT_OF_MEMORY));
        return;
    }
    r->kind = resource_kind::mesh;
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
    b.gl.DeleteBuffers(1, &r->buffer);
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

    const vgl_mesh_data* meshes = nullptr;
    static_cast<void>(vgl_frame_meshes_get(frame, &meshes, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        create_mesh(b, frame, meshes[i]);
    }

    // Imagery is not drawn yet, and the context offers no textures without imagery packs.
    const vgl_texture_data* textures = nullptr;
    static_cast<void>(vgl_frame_textures_get(frame, &textures, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        static_cast<void>(
            vgl_frame_resource_failed(frame, textures[i].resource, VGL_RESULT_NOT_SUPPORTED));
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

// The record a handle names: its address, as create_mesh set it.
const gl_resource* from_handle(std::uint64_t handle)
{
    const auto address = static_cast<std::uintptr_t>(handle);
    return reinterpret_cast<const gl_resource*>(address); // NOLINT(performance-no-int-to-ptr)
}

void draw_items(const vgl_gl_backend& b,
                const vgl_frame* frame,
                const vgl_gl_draw_info& info,
                const vgl_dmat4& projection,
                const vgl_dmat4* view)
{
    const functions& gl = b.gl;
    gl.UseProgram(b.program);
    gl.UniformMatrix4fv(b.u_projection, 1, GL_FALSE, to_float(projection).data());

    // The sun in camera space: only the view's rotation applies to a direction.
    std::array<double, 3> light{};
    const vgl_dvec3 sun = info.sun_direction;
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

    const vgl_draw_item* items = nullptr;
    std::uint32_t count = 0;
    static_cast<void>(vgl_frame_draw_list_get(frame, &items, &count));
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const vgl_draw_item& item = items[i];
        const gl_resource* mesh =
            item.mesh_handle != 0 ? from_handle(item.mesh_handle) : b.resources.find(item.mesh);
        if (mesh == nullptr)
        {
            continue;
        }
        gl.UniformMatrix4fv(b.u_model_view, 1, GL_FALSE, item.model_view);
        gl.BindVertexArray(mesh->vertex_array);
        gl.DrawElements(GL_TRIANGLES,
                        static_cast<GLsizei>(item.index_count),
                        item.index_size == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                        nullptr);
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
    // A later consumer of the context must receive every resource again.
    static_cast<void>(vgl_context_resources_reset(backend->context));
    const memory mem = backend->mem;
    mem.destroy(backend);
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
