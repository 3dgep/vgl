// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// C++ wrapper for vgl/gl.h (docs/c-api.md, section 16). Link vgl::gl.

#ifndef VGL_GL_HPP
#define VGL_GL_HPP

#include <vgl/base.hpp>
#include <vgl/context.hpp>
#include <vgl/gl.h>

#include <utility>

namespace vgl
{

// Owns a vgl_gl_backend. Destroy it on the render thread, with its OpenGL context current and
// before its context.
class gl_backend
{
public:
    gl_backend() noexcept = default;
    explicit gl_backend(vgl_gl_backend* handle) noexcept : handle_(handle) {}
    gl_backend(const gl_backend&) = delete;
    gl_backend& operator=(const gl_backend&) = delete;
    gl_backend(gl_backend&& other) noexcept : handle_(other.release()) {}
    gl_backend& operator=(gl_backend&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }
    ~gl_backend() { reset(); }

    // Creates a backend for owner on the current OpenGL context.
    [[nodiscard]] static expected<gl_backend>
    create(context& owner, const vgl_gl_backend_create_info& info) noexcept
    {
        vgl_gl_backend* handle = nullptr;
        const auto r = static_cast<result>(vgl_gl_backend_create(owner.get(), &info, &handle));
        if (failed(r))
        {
            return unexpected(r);
        }
        return gl_backend(handle);
    }

    [[nodiscard]] vgl_gl_backend* get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    [[nodiscard]] vgl_gl_backend* release() noexcept { return std::exchange(handle_, nullptr); }

    void reset(vgl_gl_backend* handle = nullptr) noexcept
    {
        static_cast<void>(vgl_gl_backend_destroy(std::exchange(handle_, handle)));
    }

    // Draws the globe into the current framebuffer. info may be null.
    [[nodiscard]] result draw(const vgl_gl_draw_info* info = nullptr) noexcept
    {
        return static_cast<result>(vgl_gl_backend_draw(handle_, info));
    }

    // Recovers from a lost OpenGL context, on the new one.
    [[nodiscard]] result context_lost() noexcept
    {
        return static_cast<result>(vgl_gl_backend_reset(handle_));
    }

private:
    vgl_gl_backend* handle_ = nullptr;
};

} // namespace vgl

#endif // VGL_GL_HPP
