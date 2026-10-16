// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The part of OpenGL 3.3 and OpenGL ES 3.0 that the backend uses: types, constants and a table
// of functions loaded through the host's loader. The backend declares these itself rather than
// including a system header, because Windows ships only OpenGL 1.1 headers, and OpenGL ES
// headers exist only where OpenGL ES does. Every value here is the same in both APIs.

#ifndef VGL_BACKENDS_GL_GL_API_HPP
#define VGL_BACKENDS_GL_GL_API_HPP

#include <vgl/gl.h>

#include <cstddef>
#include <cstdint>

// The calling convention of OpenGL functions: __stdcall on 32-bit Windows, the default
// everywhere else.
#if defined(_WIN32) && !defined(_WIN64)
#    define VGL_GL_APIENTRY __stdcall
#else
#    define VGL_GL_APIENTRY
#endif

namespace vgl::gl
{

using GLenum = std::uint32_t;
using GLboolean = std::uint8_t;
using GLbitfield = std::uint32_t;
using GLint = std::int32_t;
using GLuint = std::uint32_t;
using GLsizei = std::int32_t;
using GLfloat = float;
using GLchar = char;
using GLubyte = std::uint8_t;
using GLsizeiptr = std::ptrdiff_t;

// NOLINTBEGIN(readability-identifier-naming): the names of the OpenGL headers.
inline constexpr GLenum GL_NO_ERROR = 0;
inline constexpr GLboolean GL_FALSE = 0;
inline constexpr GLboolean GL_TRUE = 1;

inline constexpr GLenum GL_TRIANGLES = 0x0004;
inline constexpr GLenum GL_LESS = 0x0201;
inline constexpr GLenum GL_LEQUAL = 0x0203;
inline constexpr GLenum GL_GEQUAL = 0x0206;
inline constexpr GLenum GL_GREATER = 0x0204;
inline constexpr GLenum GL_FRONT = 0x0404;
inline constexpr GLenum GL_BACK = 0x0405;
inline constexpr GLenum GL_CW = 0x0900;
inline constexpr GLenum GL_CCW = 0x0901;
inline constexpr GLenum GL_CULL_FACE = 0x0B44;
inline constexpr GLenum GL_POLYGON_MODE = 0x0B40; // OpenGL only, as are the three below.
inline constexpr GLenum GL_FRONT_AND_BACK = 0x0408;
inline constexpr GLenum GL_LINE = 0x1B01;
inline constexpr GLenum GL_FILL = 0x1B02;
inline constexpr GLenum GL_CULL_FACE_MODE = 0x0B45;
inline constexpr GLenum GL_FRONT_FACE = 0x0B46;
inline constexpr GLenum GL_DEPTH_TEST = 0x0B71;
inline constexpr GLenum GL_DEPTH_WRITEMASK = 0x0B72;
inline constexpr GLenum GL_DEPTH_FUNC = 0x0B74;
inline constexpr GLenum GL_BLEND = 0x0BE2;
inline constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
inline constexpr GLenum GL_SHORT = 0x1402;
inline constexpr GLenum GL_UNSIGNED_INT = 0x1405;
inline constexpr GLenum GL_FLOAT = 0x1406;
inline constexpr GLenum GL_VERSION = 0x1F02;
inline constexpr GLenum GL_EXTENSIONS = 0x1F03;
inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
inline constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
inline constexpr GLenum GL_ARRAY_BUFFER_BINDING = 0x8894;
inline constexpr GLenum GL_STATIC_DRAW = 0x88E4;
inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
inline constexpr GLenum GL_LINK_STATUS = 0x8B82;
inline constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
inline constexpr GLenum GL_CURRENT_PROGRAM = 0x8B8D;
inline constexpr GLenum GL_NUM_EXTENSIONS = 0x821D;
inline constexpr GLenum GL_VERTEX_ARRAY_BINDING = 0x85B5;
inline constexpr GLenum GL_COPY_WRITE_BUFFER = 0x8F37;
inline constexpr GLenum GL_COPY_WRITE_BUFFER_BINDING = 0x8F37;
inline constexpr GLenum GL_PRIMITIVE_RESTART = 0x8F9D;
inline constexpr GLenum GL_PRIMITIVE_RESTART_FIXED_INDEX = 0x8D69;

// Textures and samplers.
inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum GL_RGBA = 0x1908;
inline constexpr GLenum GL_LINEAR = 0x2601;
inline constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum GL_TEXTURE_BASE_LEVEL = 0x813C;
inline constexpr GLenum GL_TEXTURE_MAX_LEVEL = 0x813D;
inline constexpr GLenum GL_RGBA8 = 0x8058;
inline constexpr GLenum GL_SRGB8_ALPHA8 = 0x8C43;
inline constexpr GLenum GL_TEXTURE0 = 0x84C0;
inline constexpr GLenum GL_ACTIVE_TEXTURE = 0x84E0;
inline constexpr GLenum GL_TEXTURE_BINDING_2D = 0x8069;
inline constexpr GLenum GL_SAMPLER_BINDING = 0x8919;
inline constexpr GLenum GL_TEXTURE_MAX_ANISOTROPY = 0x84FE;     // EXT_texture_filter_anisotropic
inline constexpr GLenum GL_MAX_TEXTURE_MAX_ANISOTROPY = 0x84FF; // and OpenGL 4.6.

// Pixel unpacking, which the host may have changed.
inline constexpr GLenum GL_UNPACK_ROW_LENGTH = 0x0CF2;
inline constexpr GLenum GL_UNPACK_SKIP_ROWS = 0x0CF3;
inline constexpr GLenum GL_UNPACK_SKIP_PIXELS = 0x0CF4;
inline constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum GL_PIXEL_UNPACK_BUFFER = 0x88EC;
inline constexpr GLenum GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF;

// Clip control: OpenGL 4.5, GL_ARB_clip_control and GL_EXT_clip_control share these values.
inline constexpr GLenum GL_LOWER_LEFT = 0x8CA1;
inline constexpr GLenum GL_CLIP_ORIGIN = 0x935C;
inline constexpr GLenum GL_CLIP_DEPTH_MODE = 0x935D;
inline constexpr GLenum GL_NEGATIVE_ONE_TO_ONE = 0x935E;
inline constexpr GLenum GL_ZERO_TO_ONE = 0x935F;
// NOLINTEND(readability-identifier-naming)

// The functions the backend calls: X(return type, name without "gl", parameter list).
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define VGL_GL_FUNCTIONS(X)                                                                        \
    X(const GLubyte*, GetString, (GLenum name))                                                    \
    X(const GLubyte*, GetStringi, (GLenum name, GLuint index))                                     \
    X(void, GetIntegerv, (GLenum pname, GLint * data))                                             \
    X(GLboolean, IsEnabled, (GLenum cap))                                                          \
    X(void, Enable, (GLenum cap))                                                                  \
    X(void, Disable, (GLenum cap))                                                                 \
    X(void, DepthFunc, (GLenum func))                                                              \
    X(void, DepthMask, (GLboolean flag))                                                           \
    X(void, CullFace, (GLenum mode))                                                               \
    X(void, FrontFace, (GLenum mode))                                                              \
    X(GLuint, CreateShader, (GLenum type))                                                         \
    X(void,                                                                                        \
      ShaderSource,                                                                                \
      (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length))            \
    X(void, CompileShader, (GLuint shader))                                                        \
    X(void, GetShaderiv, (GLuint shader, GLenum pname, GLint * params))                            \
    X(void, GetShaderInfoLog, (GLuint shader, GLsizei size, GLsizei * length, GLchar * log))       \
    X(void, DeleteShader, (GLuint shader))                                                         \
    X(GLuint, CreateProgram, (void))                                                               \
    X(void, AttachShader, (GLuint program, GLuint shader))                                         \
    X(void, DetachShader, (GLuint program, GLuint shader))                                         \
    X(void, LinkProgram, (GLuint program))                                                         \
    X(void, GetProgramiv, (GLuint program, GLenum pname, GLint * params))                          \
    X(void, GetProgramInfoLog, (GLuint program, GLsizei size, GLsizei * length, GLchar * log))     \
    X(void, DeleteProgram, (GLuint program))                                                       \
    X(void, UseProgram, (GLuint program))                                                          \
    X(GLint, GetUniformLocation, (GLuint program, const GLchar* name))                             \
    X(void, Uniform1f, (GLint location, GLfloat v0))                                               \
    X(void, Uniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2))                       \
    X(void,                                                                                        \
      UniformMatrix4fv,                                                                            \
      (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value))                  \
    X(void, GenBuffers, (GLsizei n, GLuint * buffers))                                             \
    X(void, DeleteBuffers, (GLsizei n, const GLuint* buffers))                                     \
    X(void, BindBuffer, (GLenum target, GLuint buffer))                                            \
    X(void, BufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))          \
    X(void, GenVertexArrays, (GLsizei n, GLuint * arrays))                                         \
    X(void, DeleteVertexArrays, (GLsizei n, const GLuint* arrays))                                 \
    X(void, BindVertexArray, (GLuint array))                                                       \
    X(void, EnableVertexAttribArray, (GLuint index))                                               \
    X(void,                                                                                        \
      VertexAttribPointer,                                                                         \
      (GLuint index,                                                                               \
       GLint size,                                                                                 \
       GLenum type,                                                                                \
       GLboolean normalized,                                                                       \
       GLsizei stride,                                                                             \
       const void* pointer))                                                                       \
    X(void, DrawElements, (GLenum mode, GLsizei count, GLenum type, const void* indices))          \
    X(void, GetFloatv, (GLenum pname, GLfloat * data))                                             \
    X(void, Uniform1i, (GLint location, GLint v0))                                                 \
    X(void, Uniform4fv, (GLint location, GLsizei count, const GLfloat* value))                     \
    X(void, GenTextures, (GLsizei n, GLuint * textures))                                           \
    X(void, DeleteTextures, (GLsizei n, const GLuint* textures))                                   \
    X(void, BindTexture, (GLenum target, GLuint texture))                                          \
    X(void, ActiveTexture, (GLenum texture))                                                       \
    X(void, TexParameteri, (GLenum target, GLenum pname, GLint param))                             \
    X(void,                                                                                        \
      TexImage2D,                                                                                  \
      (GLenum target,                                                                              \
       GLint level,                                                                                \
       GLint internalformat,                                                                       \
       GLsizei width,                                                                              \
       GLsizei height,                                                                             \
       GLint border,                                                                               \
       GLenum format,                                                                              \
       GLenum type,                                                                                \
       const void* pixels))                                                                        \
    X(void, PixelStorei, (GLenum pname, GLint param))                                              \
    X(void, GenSamplers, (GLsizei count, GLuint * samplers))                                       \
    X(void, DeleteSamplers, (GLsizei count, const GLuint* samplers))                               \
    X(void, BindSampler, (GLuint unit, GLuint sampler))                                            \
    X(void, SamplerParameteri, (GLuint sampler, GLenum pname, GLint param))                        \
    X(void, SamplerParameterf, (GLuint sampler, GLenum pname, GLfloat param))

// The parameter list cannot be parenthesized again: it is part of the declarator.
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define VGL_GL_DECLARE_FUNCTION(ret, name, params) ret(VGL_GL_APIENTRY* name) params = nullptr;

struct functions
{
    VGL_GL_FUNCTIONS(VGL_GL_DECLARE_FUNCTION)

    // Optional: glClipControl or glClipControlEXT, when the device has clip control.
    void(VGL_GL_APIENTRY* ClipControl)(GLenum origin, GLenum depth) = nullptr;

    // Optional: glPolygonMode, for the wireframe debug view; OpenGL only.
    void(VGL_GL_APIENTRY* PolygonMode)(GLenum face, GLenum mode) = nullptr;
};

#undef VGL_GL_DECLARE_FUNCTION
// NOLINTEND(cppcoreguidelines-macro-usage)

// What the current OpenGL context is and can do.
struct device
{
    bool es = false; // OpenGL ES rather than OpenGL.
    int major = 0;
    int minor = 0;
    bool primitive_restart = false;             // GL_PRIMITIVE_RESTART exists (OpenGL).
    bool primitive_restart_fixed_index = false; // GL_PRIMITIVE_RESTART_FIXED_INDEX exists.
    float max_anisotropy = 1.0f;                // Above 1 with anisotropic filtering.
};

// Loads the functions and identifies the device. On failure, writes the name of what is
// missing, or the version found, to message (a buffer of message_size bytes) and returns
// VGL_RESULT_NOT_SUPPORTED.
[[nodiscard]] vgl_result load(vgl_gl_get_proc_address_fn get_proc_address,
                              void* user_data,
                              bool want_clip_control,
                              functions& out_functions,
                              device& out_device,
                              char* message,
                              std::size_t message_size) noexcept;

} // namespace vgl::gl

#endif // VGL_BACKENDS_GL_GL_API_HPP
