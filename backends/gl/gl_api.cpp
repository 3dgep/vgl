// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "gl_api.hpp"

#include <cstdio>
#include <cstring>

namespace vgl::gl
{

namespace
{

// Reads "major.minor" from a version string such as "4.6.0 NVIDIA 560.94" or "OpenGL ES 3.2
// Mesa 25.2.8".
bool parse_version(const char* text, int& major, int& minor) noexcept
{
    while (*text != '\0' && (*text < '0' || *text > '9'))
    {
        ++text;
    }
    major = 0;
    minor = 0;
    if (*text == '\0')
    {
        return false;
    }
    while (*text >= '0' && *text <= '9')
    {
        major = major * 10 + (*text++ - '0');
    }
    if (*text++ != '.')
    {
        return false;
    }
    while (*text >= '0' && *text <= '9')
    {
        minor = minor * 10 + (*text++ - '0');
    }
    return true;
}

bool has_extension(const functions& f, const char* name) noexcept
{
    GLint count = 0;
    f.GetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i)
    {
        const auto* e =
            reinterpret_cast<const char*>(f.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (e != nullptr && std::strcmp(e, name) == 0)
        {
            return true;
        }
    }
    return false;
}

bool at_least(const device& d, int major, int minor) noexcept
{
    return d.major > major || (d.major == major && d.minor >= minor);
}

} // namespace

vgl_result load(vgl_gl_get_proc_address_fn get_proc_address,
                void* user_data,
                bool want_clip_control,
                functions& out_functions,
                device& out_device,
                char* message,
                std::size_t message_size) noexcept
{
    functions f;
    const char* missing = nullptr;
    // NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define VGL_GL_LOAD_FUNCTION(ret, name, params)                                                    \
    f.name = reinterpret_cast<decltype(f.name)>(get_proc_address(user_data, "gl" #name));          \
    if (f.name == nullptr && missing == nullptr)                                                   \
    {                                                                                              \
        missing = "gl" #name;                                                                      \
    }
    VGL_GL_FUNCTIONS(VGL_GL_LOAD_FUNCTION)
#undef VGL_GL_LOAD_FUNCTION
    // NOLINTEND(cppcoreguidelines-macro-usage)

    if (f.GetString == nullptr)
    {
        static_cast<void>(
            std::snprintf(message, message_size, "the loader returned no glGetString"));
        return VGL_RESULT_NOT_SUPPORTED;
    }
    const auto* version = reinterpret_cast<const char*>(f.GetString(GL_VERSION));
    if (version == nullptr)
    {
        static_cast<void>(std::snprintf(
            message, message_size, "glGetString(GL_VERSION) failed; is a context current?"));
        return VGL_RESULT_NOT_SUPPORTED;
    }
    device d;
    d.es = std::strncmp(version, "OpenGL ES", 9) == 0;
    if (!parse_version(version, d.major, d.minor) || !at_least(d, 3, d.es ? 0 : 3))
    {
        static_cast<void>(
            std::snprintf(message,
                          message_size,
                          "the context has version \"%s\"; it needs OpenGL 3.3 or OpenGL ES 3.0",
                          version));
        return VGL_RESULT_NOT_SUPPORTED;
    }
    if (missing != nullptr)
    {
        static_cast<void>(
            std::snprintf(message, message_size, "the loader returned no %s", missing));
        return VGL_RESULT_NOT_SUPPORTED;
    }
    if (!d.es)
    {
        f.PolygonMode =
            reinterpret_cast<decltype(f.PolygonMode)>(get_proc_address(user_data, "glPolygonMode"));
    }
    d.primitive_restart = !d.es;
    d.primitive_restart_fixed_index = d.es || at_least(d, 4, 3);
    if ((!d.es && at_least(d, 4, 6)) || has_extension(f, "GL_EXT_texture_filter_anisotropic") ||
        has_extension(f, "GL_ARB_texture_filter_anisotropic"))
    {
        f.GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &d.max_anisotropy);
    }

    if (want_clip_control)
    {
        const char* name = nullptr;
        if (d.es)
        {
            name = has_extension(f, "GL_EXT_clip_control") ? "glClipControlEXT" : nullptr;
        }
        else if (at_least(d, 4, 5) || has_extension(f, "GL_ARB_clip_control"))
        {
            name = "glClipControl";
        }
        if (name != nullptr)
        {
            f.ClipControl =
                reinterpret_cast<decltype(f.ClipControl)>(get_proc_address(user_data, name));
        }
        if (f.ClipControl == nullptr)
        {
            static_cast<void>(std::snprintf(
                message,
                message_size,
                "the reversed-Z convention needs clip control, which \"%s\" lacks; use the "
                "logarithmic convention",
                version));
            return VGL_RESULT_NOT_SUPPORTED;
        }
    }
    out_functions = f;
    out_device = d;
    return VGL_RESULT_SUCCESS;
}

} // namespace vgl::gl
