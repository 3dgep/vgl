// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// C++ wrapper for vgl/d3d11.h (docs/c-api.md, section 16). Link vgl::d3d11.

#ifndef VGL_D3D11_HPP
#define VGL_D3D11_HPP

#include <vgl/base.hpp>
#include <vgl/context.hpp>
#include <vgl/d3d11.h>

#include <utility>

namespace vgl
{

// Owns a vgl_d3d11_backend. Destroy it on the thread that owns its device context, before its
// context.
class d3d11_backend
{
public:
    d3d11_backend() noexcept = default;
    explicit d3d11_backend(vgl_d3d11_backend* handle) noexcept : handle_(handle) {}
    d3d11_backend(const d3d11_backend&) = delete;
    d3d11_backend& operator=(const d3d11_backend&) = delete;
    d3d11_backend(d3d11_backend&& other) noexcept : handle_(other.release()) {}
    d3d11_backend& operator=(d3d11_backend&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }
    ~d3d11_backend() { reset(); }

    // Creates a backend for owner on the device that info names.
    [[nodiscard]] static expected<d3d11_backend>
    create(context& owner, const vgl_d3d11_backend_create_info& info) noexcept
    {
        vgl_d3d11_backend* handle = nullptr;
        const auto r = static_cast<result>(vgl_d3d11_backend_create(owner.get(), &info, &handle));
        if (failed(r))
        {
            return unexpected(r);
        }
        return d3d11_backend(handle);
    }

    [[nodiscard]] vgl_d3d11_backend* get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    [[nodiscard]] vgl_d3d11_backend* release() noexcept { return std::exchange(handle_, nullptr); }

    void reset(vgl_d3d11_backend* handle = nullptr) noexcept
    {
        static_cast<void>(vgl_d3d11_backend_destroy(std::exchange(handle_, handle)));
    }

    // Draws the globe into the bound render target. info may be null.
    [[nodiscard]] result draw(const vgl_d3d11_draw_info* info = nullptr) noexcept
    {
        return static_cast<result>(vgl_d3d11_backend_draw(handle_, info));
    }

    // Recovers from a removed device, on the new one.
    [[nodiscard]] result device_lost(ID3D11Device* device,
                                     ID3D11DeviceContext* device_context) noexcept
    {
        return static_cast<result>(vgl_d3d11_backend_reset(handle_, device, device_context));
    }

private:
    vgl_d3d11_backend* handle_ = nullptr;
};

} // namespace vgl

#endif // VGL_D3D11_HPP
